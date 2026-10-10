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

#define HOME_RETRY_MS  20000
#define DROP_TIMEOUT_MS 15000

class Brain {
public:
    Brain(Link& link, Settings settings, std::string settings_path, uint32_t rng_seed);
    ~Brain();

    void update(uint64_t now_ms);           // call every UI frame
    void onButton(uint8_t btn);             // same path as a forwarded CMD_BUTTON
    // Start a game from the UI's own menu. Only works in MENU (gantry homed and ready).
    bool startGame(int mode);

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

    void say(const std::string& s) { if (log) log(s); }
    void setFace(FaceEvent f) { face = f; faceSeq++; }
    void enter(State s);
    void handleFrame(const Frame& f);
    void beginGame(int mode);
    void beginHumanTurn();
    void beginAiTurn();
    void sendHome();
    void sendDrop(int col, int mover);
    void sendSelector();
    void fault(uint8_t code, const char* why);
    void finishSearch();
    void stopSearch();
    int  nearestLegal(int start) const;
    void onDone(int col);
};
