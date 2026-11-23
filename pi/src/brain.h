#pragma once
// ============================================================
// brain.h -- Game Brain state machine (no rendering, no SDL)
// ============================================================
// Owns: game state, menu, bot, faces, fault state, settings.
// The UI only READS public state and draws it. Input arrives either as
// CMD_BUTTON frames from the ESP32 or from onButton() (laptop keyboard).
//
// Time is passed in (update(now_ms)) so tests can run a whole game in
// simulated time.
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include "game.h"
#include "link.h"
#include "reaction.h"
#include "settings.h"

enum class State { HOMING, MENU, HUMAN_TURN, AI_TURN, AWAITING_DROP, GAME_OVER, FAULT };
const char* stateName(State s);

enum Button : uint8_t { BTN_LEFT = 0, BTN_RIGHT = 1, BTN_CONFIRM = 2 };

// Fault codes shown on the fault screen. 0x01-0x04 come from the ESP32
// (C4Error); these two are detected on the Pi.
#define FAULT_DESYNC   0xFF   // CMD_DONE echoed the wrong column
#define FAULT_TIMEOUT  0xFD   // no CMD_DONE/CMD_ERROR within DROP_TIMEOUT_MS
#define FAULT_DEAF     0xFC   // Motion Controller keeps beaconing READY: it can't hear the Pi
#define FAULT_MC_RESET 0xFB   // Motion Controller restarted in the middle of a game
#define DEAF_READY_LIMIT 3    // stray READYs (outside HOMING) before FAULT_DEAF
#define LINK_LOG_MAX   60
#define RESTART_FALLBACK_MS 3000   // no DBG_BOOT this long after CMD_RESET: send CMD_HOME instead
#define AUTO_RESTARTS  1      // homing fails: restart the Motion Controller this many times before faulting
#define SILENT_AFTER_MS 6000  // nothing at all from the board this long: it is not running
#define SILENT_BREAK_EVERY_MS 8000
#define SILENT_BREAK_MAX 5    // reset attempts over the TX wire (needs the 1 k resistor to EN)

#define HOME_RETRY_MS  20000
#define DROP_TIMEOUT_MS 15000     // with no sign of life (any frame) from the gantry; progress frames reset it

class Brain {
public:
    Brain(Link& link, Settings settings, std::string settings_path, uint32_t rng_seed);
    ~Brain();

    void update(uint64_t now_ms);           // call every UI frame
    void onButton(uint8_t btn);             // same path as a forwarded CMD_BUTTON
    // Start a game from the UI's own menu. Only works in MENU (gantry homed and ready).
    // first = who moves first (decided by the coin toss): 0 = RED (R.O.G.E.R in
    // AI modes, Player One in 1v1), 1 = BLACK (the human / Player Two).
    bool startGame(int mode, int first = 0);
    // Start over from homing: clears a fault and re-sends CMD_HOME. Used when
    // switching between the gantry and screen-only play.
    void rehome();
    void saveScreenMode(bool on, uint8_t input) {
        settings_.screen_only = on; settings_.screen_input = input; settings_.save(settings_path_);
    }
    bool screenOnlySetting() const { return settings_.screen_only; }
    uint8_t screenInputSetting() const { return settings_.screen_input; }
    int  pendingCol() const { return pending_col_; }   // column of the drop in progress

    // ---- state the UI reads ----
    State     state = State::HOMING;
    Board     board;
    FaceEvent face = FACE_IDLE;             // which Roger clip should play
    uint32_t  faceSeq = 0;                  // bumps on every face change, even to the same face
    int       mode = 0;                     // 0 Pass&Play, 1 Noob, 2 Pro, 3 Hacker
    int       menuIndex = 2;                // highlighted menu row
    bool      inConfirm = false;            // "You selected X / Start / Back" screen
    int       confirmIndex = 0;             // 0 = Start Game, 1 = Back
    int       selectedCol = 0;              // column selector on a human turn
    int       winner = -1;                  // -1 none, 0 RED, 1 BLACK, 2 draw
    uint8_t   faultCode = 0;
    int32_t   lastAiValue = 0;
    uint32_t  lastAiNodes = 0;
    bool      searching() const { return searching_; }
    bool      shutdownRequested = false;    // shutdown button on Motion Controller held

    // ---- link debugging ----
    struct LinkEvent { uint64_t t; bool fromMc; std::string text; };
    std::deque<LinkEvent> linkLog;          // newest at the back, both directions
    struct McStatus {
        int fw = -1;                        // firmware version from DBG_BOOT/DBG_STATUS (-1: never said)
        uint64_t lastHeard = 0;             // last proof it HEARD the Pi (ack, status, DONE/ERROR)
        uint64_t lastFrame = 0;             // last frame of any kind from it
        bool homed = false, limit = false, statusKnown = false;
        int badFrames = -1;                 // corrupted frames it got from the Pi
        int resetReason = -1;               // from DBG_BOOT
        int lastMoveCol = -1;
    } mc;
    int strayReady() const { return strayReady_; }
    // True when the Motion Controller has never sent a single frame since the
    // game started: the ESP32 is not running (it needs its EN button), which
    // no firmware can fix. The menu says so.
    bool boardSilent() const { return state == State::HOMING && mc.lastFrame == 0 && firstNow_ && now_ - firstNow_ >= SILENT_AFTER_MS; }
    int  resetTries() const { return silentBreaks_; }
    std::string lastBoardEvent() const {
        for (auto it = linkLog.rbegin(); it != linkLog.rend(); ++it) if (it->fromMc) return it->text;
        return "";
    }
    uint64_t now() const { return now_; }
    uint64_t stateSince() const { return state_since_; }
    const LinkCounters& linkCounters() const { return link_.counters(); }
    // Fault recovery and debug tools (MENU, HOMING or FAULT only; never mid-game).
    void resetToMenu() { rehome(); }        // clear the fault, re-home, back to the mode menu
    void restartMotionController();         // CMD_RESET: the board reboots, homes, then READY
    bool debugPing();
    bool debugMove(int col);
    bool debugDrop(int col, int color);
    bool debugAllowed() const { return state == State::MENU || state == State::HOMING || state == State::FAULT; }
    static const char* resetReasonName(int r);

    std::function<void(const std::string&)> log;   // optional
    // If set, every CMD_BUTTON from the ESP32 goes here first; return true to
    // consume it (the UI uses this to drive its own menus).
    std::function<bool(uint8_t)> buttonHook;

private:
    Link& link_;
    Settings settings_;
    std::string settings_path_;
    std::mt19937 rng_;
    ReactionClassifier reaction_;

    uint64_t now_ = 0, state_since_ = 0;
    int pending_col_ = -1, pending_mover_ = 0;

    // search thread
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> done_{false};
    bool searching_ = false;
    SearchResult result_{};
    bool resultReady_ = false;    // result_ is a finished search of searchedState_
    bool reactPending_ = false;   // the running search is judging the human's move
    C4State searchedState_{};

    void say(const std::string& s) { if (log) log(s); }
    int strayReady_ = 0;
    uint64_t dropAliveAt_ = 0;      // last sign of life from the gantry during a drop
    uint64_t restartSentAt_ = 0;  // CMD_RESET sent; v1 firmware ignores it, so fall back to CMD_HOME
    int autoRestartsLeft_ = AUTO_RESTARTS;
    bool faultFromHoming_ = false; // the fault is a failed home (not mid-game): a later READY clears it
    uint64_t firstNow_ = 0, lastSilentBreak_ = 0;
    int silentBreaks_ = 0;
    void tx(uint8_t cmd, const uint8_t* p, uint8_t n, bool hardware = false);
    void logLink(bool fromMc, const std::string& s);
    std::string describe(const Frame& f) const;
    void setFace(FaceEvent f) { face = f; faceSeq++; }
    void enter(State s);
    void handleFrame(const Frame& f);
    void beginGame(int mode, int first = 0);
    void beginHumanTurn();
    void beginAiTurn();
    void sendHome();
    void sendDrop(int col, int mover);
    void sendSelector();
    void fault(uint8_t code, const char* why);
    void startSearch(const Board& position, bool reactToHumanMove);
    void harvestSearch();         // search finished: judge the human's move, keep the result
    void playAiMove();
    void stopSearch();
    int  nearestLegal(int start) const;
    void onDone(int col);
};
