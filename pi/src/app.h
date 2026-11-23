#pragma once
// ============================================================
// app.h -- the screen layer (C++ port of Finn's connect4.py + face player hookup)
// ============================================================
// Screens: Finn's main menu, a "You selected X / Start game / Back" confirm
// screen, the coin toss (call it, then the flip), Options (Screen only, Link
// debug, GIF test, Shut down), Statistics, and the in-game screens:
//   vs R.O.G.E.R : his face fills the screen, with turn status and the column selector
//   1v1          : Finn's "Player One's turn" screen plus the selector
//   game over    : win/loss GIF once, then the results popup with Play again
//   fault        : red screen with what went wrong and ways back without a power cycle
// The game itself is still entirely Brain's state machine; this layer only
// draws it, plays the face that matches Brain's face event, and records stats.
//
// Only three physical buttons exist (left, right, select), so in menus
// left/right move up/down and every screen has a way back without an Esc key.
#include <random>
#include <string>
#include "brain.h"
#include "faces.h"
#include "gfx.h"
#include "stats.h"

enum Act { A_UP, A_DOWN, A_SELECT, A_BACK, A_TEST, A_CLEAR, A_BOARD, A_QUIT };

enum class Screen { MENU, CONFIRM, COINCALL, COINTOSS, OPTIONS, SCREENMODE, SHUTDOWN, STATS, GIFTEST, GAME, LINKDEBUG, SERVOCAL };

class MotionController;

class App {
public:
    App(Brain& brain, FacePlayer& faces, Stats& stats, std::string statsPath, bool sim, SwitchLink* sw = nullptr);
    bool screenOnly() const { return sw_ && sw_->screenOnly; }
    // Screen-only on/off and which device plays the game (0 = Pi keyboard,
    // 1 = Motion Controller buttons). Menus always accept both.
    void setScreenMode(bool on, uint8_t input);
    uint8_t screenInput() const { return screenInput_; }
    void testFace(FaceEvent f) { applyFace(f); }   // tests only: same path as a face from the game
    // Coin toss (tests read these): call and result are 0 = heads, 1 = tails;
    // first is who moves first, 0 = RED (R.O.G.E.R / Player One), 1 = BLACK (you / Player Two).
    int tossCall() const { return tossCall_; }
    int tossResult() const { return tossResult_; }
    int tossFirst() const { return tossFirst_; }
    static const int TOSS_MS = 1800, TOSS_HOLD_MS = 1800;   // coin in the air; result shown before the game starts
    // Game over: the reaction finishes its pass, R.O.G.E.R's win/loss GIF plays
    // once, then the results popup ("YOU LOST" / "YOU WON!" / "PLAYER ONE
    // WINS!" ...) with Play again, which goes back to mode selection.
    enum class Over { NONE, WAIT_REACTION, GIF, HOLD, POPUP };
    Over over() const { return over_; }
    int menuSel() const { return sel_; }        // tests: highlighted main-menu item
    int optionSel() const { return optSel_; }   // tests: highlighted Options item
    static const int OVER_HOLD_MS = 2000, OVER_MAX_MS = 20000;

    void key(Act a);                 // keyboard on the Pi
    bool espButton(uint8_t btn);     // Brain::buttonHook: true = the menu used it

    // Call every loop. Advances the face animation and redraws into `cv`.
    // Returns true when `cv` changed and should be presented.
    bool frame(uint64_t now, Canvas& cv);

    bool quit = false;
    bool shutdownRequested = false;
    bool powerOffAllowed = true;     // main() checks the passwordless rule at startup
    bool showBoard = false;          // small board in the corner (on by default in --sim)
    std::string faceMessage;         // what the face loader said
    Screen screen = Screen::MENU;
    // The Pi-GPIO gantry (no ESP32): enables Options > Servo calibration.
    // nullptr = no gantry on this Pi (--sim, --serial): the screen says so.
    void setGantry(MotionController* mc) { mc_ = mc; }
    std::string gantryInfo;          // Link debug: where the pins came from, or what is missing
    bool chordSelect = false;        // select = left + right together: the on-screen hints say so
    int calSel() const { return calSel_; }           // tests
    bool calEditing() const { return calEditing_; }  // tests
    int calValue(int i) const { return calVals_[i]; } // tests
    // A message box over whatever screen is showing (e.g. power-off refused).
    void showBanner(const std::string& line1, const std::string& line2, uint64_t now, int ms) {
        banner1_ = line1; banner2_ = line2; bannerUntil_ = now + (uint64_t)ms;
    }

    // drawing (public so tests can render any screen)
    void draw(Canvas& cv, uint64_t now);
    void drawShuttingDown(Canvas& cv);

private:
    Brain& b_;
    FacePlayer& faces_;
    Stats& stats_;
    std::string statsPath_;
    bool sim_;
    SwitchLink* sw_;
    uint8_t screenInput_ = 1;      // screen-only controls: 1 = Motion Controller buttons (default), 0 = keyboard
    int smSel_ = 1;
    uint64_t lastNow_ = 0, wrongInputAt_ = 0;   // "use the other controls" notice
    uint64_t dropStart_ = 0;
    int dropCol_ = -1, dropColor_ = 0;
    int sel_ = 2;            // main menu: item index (0 1v1, 1 Noob, 2 Pro, 3 Hacker, 4 Options, 5 Statistics)
    int confirmMode_ = 2, confirmSel_ = 0;
    int optSel_ = 0, shutSel_ = 1;
    int coinSel_ = 0;                          // 0 Heads, 1 Tails, 2 Back
    int tossCall_ = 0, tossResult_ = 0, tossFirst_ = 0;
    uint64_t tossStart_ = 0;                   // 0 = starts on the next frame
    Over over_ = Over::NONE;
    uint64_t overSince_ = 0;
    std::string endGroup_;
    int faultSel_ = 0;                         // fault screen: Back to menu, Restart gantry, Link debug, Options
    int dbgSel_ = 0, dbgCol_ = 3;              // Link debug: Ping, Home, Column, Drop here, Restart, Back
    uint64_t dbgLastPing_ = 0;
    Screen dbgReturn_ = Screen::OPTIONS;
    std::string banner1_, banner2_;
    uint64_t bannerUntil_ = 0;
    std::mt19937 coinRng_{std::random_device{}()};
    bool confirmClear_ = false;
    std::string notice_;     // one-line message on the confirm screen
    uint32_t lastFaceSeq_ = 0xFFFFFFFF;
    State lastState_ = State::HOMING;
    bool faceDirty_ = true;
    std::string lastSig_;

    void handle(Act a);
    void syncWithBrain(uint64_t now);
    void applyFace(FaceEvent f);
    bool isReaction(const std::string& group) const;
    std::string groupFor(FaceEvent f) const;
    void recordResult();
    std::string signature(uint64_t now) const;
    std::string statusText(uint64_t now) const;

    void drawMenu(Canvas& cv, uint64_t now);
    void drawItem(Canvas& cv, int y, const std::string& label, bool selected, int cx, int boxW = 300);
    void drawConfirm(Canvas& cv);
    void drawCoinCall(Canvas& cv);
    void drawCoinToss(Canvas& cv, uint64_t now);
    void drawCoin(Canvas& cv, int cx, int cy, int r, double angle);
    void startTossedGame();
    void startGameOver(uint64_t now);
    void advanceGameOver(uint64_t now);
    void gameOverInput(Act a);
    void drawGameOverPopup(Canvas& cv);
    void drawOptions(Canvas& cv);
    void drawScreenMode(Canvas& cv);
    int  screenModeCount() const { return screenOnly() ? 4 : 3; }
    void drawShutdown(Canvas& cv);
    void drawStats(Canvas& cv);
    void drawGifTest(Canvas& cv);
    void drawGame(Canvas& cv, uint64_t now);
    void drawPvp(Canvas& cv);
    void drawFault(Canvas& cv);
    void drawLinkDebug(Canvas& cv, uint64_t now);
    void drawServoCal(Canvas& cv);
    void openServoCal();
    void closeServoCal();
    void servoCalInput(Act a);
    MotionController* mc_ = nullptr;
    int calSel_ = 0;                 // 0-5 values (rest, red sel/push, black sel/push, wait), 6 test red, 7 test black, 8 save, 9 back
    bool calEditing_ = false;
    int calVals_[6] = {103, 70, 100, 135, 110, 250};
    int calSaveSeen_ = 0;
    void openLinkDebug();
    void drawSelector(Canvas& cv);
    void drawMiniBoard(Canvas& cv);
    void drawBigBoard(Canvas& cv, uint64_t now);
    void drawScreenOnly(Canvas& cv, uint64_t now);
    void hint(Canvas& cv, const std::string& s);
    void statRow(Canvas& cv, int y, const std::string& label, const WLD& r);
};
