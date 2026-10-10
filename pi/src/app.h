#pragma once
// ============================================================
// app.h -- the screen layer (C++ port of Finn's connect4.py + face player hookup)
// ============================================================
// Screens: Finn's main menu, a "You selected X / Start game / Back" confirm
// screen, Options (GIF test, Shut down), Statistics, and the in-game screens:
//   vs R.O.G.E.R : his face fills the screen, with turn status, the column
//                  selector and the game-over banner drawn on top
//   1v1          : Finn's "Player One's turn" screen plus the selector
//   fault        : red screen with what went wrong
// The game itself is still entirely Brain's state machine; this layer only
// draws it, plays the face that matches Brain's face event, and records stats.
//
// Only three physical buttons exist (left, right, select), so in menus
// left/right move up/down and every screen has a way back without an Esc key.
#include <string>
#include "brain.h"
#include "faces.h"
#include "gfx.h"
#include "stats.h"

enum Act { A_UP, A_DOWN, A_SELECT, A_BACK, A_TEST, A_CLEAR, A_BOARD, A_QUIT };

enum class Screen { MENU, CONFIRM, OPTIONS, SCREENMODE, SHUTDOWN, STATS, GIFTEST, GAME };

class App {
public:
    App(Brain& brain, FacePlayer& faces, Stats& stats, std::string statsPath, bool sim, SwitchLink* sw = nullptr);
    bool screenOnly() const { return sw_ && sw_->screenOnly; }
    // Screen-only on/off and which device plays the game (0 = Pi keyboard,
    // 1 = Motion Controller buttons). Menus always accept both.
    void setScreenMode(bool on, uint8_t input);
    uint8_t screenInput() const { return screenInput_; }

    void key(Act a);                 // keyboard on the Pi
    bool espButton(uint8_t btn);     // Brain::buttonHook: true = the menu used it

    // Call every loop. Advances the face animation and redraws into `cv`.
    // Returns true when `cv` changed and should be presented.
    bool frame(uint64_t now, Canvas& cv);

    bool quit = false;
    bool shutdownRequested = false;
    bool showBoard = false;          // small board in the corner (on by default in --sim)
    std::string faceMessage;         // what the face loader said
    Screen screen = Screen::MENU;

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
    uint8_t screenInput_ = 0;
    int smSel_ = 0;
    uint64_t lastNow_ = 0, wrongInputAt_ = 0;   // "use the other controls" notice
    uint64_t dropStart_ = 0;
    int dropCol_ = -1, dropColor_ = 0;
    int sel_ = 2;            // main menu: item index (0 1v1, 1 Noob, 2 Pro, 3 Hacker, 4 Options, 5 Statistics)
    int confirmMode_ = 2, confirmSel_ = 0;
    int optSel_ = 0, shutSel_ = 1;
    bool confirmClear_ = false;
    std::string notice_;     // one-line message on the confirm screen
    uint32_t lastFaceSeq_ = 0xFFFFFFFF;
    State lastState_ = State::HOMING;
    bool faceDirty_ = true;
    std::string lastSig_;

    void handle(Act a);
    void syncWithBrain(uint64_t now);
    void applyFace(FaceEvent f);
    std::string groupFor(FaceEvent f) const;
    void recordResult();
    std::string signature(uint64_t now) const;
    std::string statusText(uint64_t now) const;

    void drawMenu(Canvas& cv);
    void drawItem(Canvas& cv, int y, const std::string& label, bool selected, int cx, int boxW = 300);
    void drawConfirm(Canvas& cv);
    void drawOptions(Canvas& cv);
    void drawScreenMode(Canvas& cv);
    int  screenModeCount() const { return screenOnly() ? 4 : 3; }
    void drawShutdown(Canvas& cv);
    void drawStats(Canvas& cv);
    void drawGifTest(Canvas& cv);
    void drawGame(Canvas& cv, uint64_t now);
    void drawPvp(Canvas& cv);
    void drawFault(Canvas& cv);
    void drawSelector(Canvas& cv);
    void drawMiniBoard(Canvas& cv);
    void drawBigBoard(Canvas& cv, uint64_t now);
    void drawScreenOnly(Canvas& cv, uint64_t now);
    void hint(Canvas& cv, const std::string& s);
    void statRow(Canvas& cv, int y, const std::string& label, const WLD& r);
};
