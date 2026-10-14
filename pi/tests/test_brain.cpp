// Whole-game tests of the Brain against SimLink, in simulated time.
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>
#include "../src/brain.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("  FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Rig {
    SimLink sim; Brain* b; uint64_t t = 0;
    std::string logbuf;
    Rig(int start_mode = 2, bool fail_home = false) {
        sim.fail_home = fail_home;
        Settings s; s.last_mode = (uint8_t)start_mode;
        b = new Brain(sim, s, "/tmp/c4_test_settings.conf", 1);
        b->log = [this](const std::string& m) { logbuf += m + "\n"; };
    }
    ~Rig() { delete b; }
    // advance simulated time in 10 ms steps; real sleep only while a search runs
    void run(uint64_t ms) { for (uint64_t e = t + ms; t < e; t += 10) { b->update(t); if (b->searching()) usleep(1000); } }
    bool until(State s, uint64_t max_ms = 60000) {
        for (uint64_t e = t + max_ms; t < e; t += 10) { b->update(t); if (b->state == s) return true; if (b->searching()) usleep(1000); }
        return false;
    }
    void press(uint8_t btn) { sim.pushButton(btn); run(20); }
    void startMode(int m) {            // from MENU: navigate to mode m, confirm, start
        while (b->menuIndex != m) press(BTN_RIGHT);
        press(BTN_CONFIRM); press(BTN_CONFIRM);
    }
};

static void test_boot_and_menu() {
    printf("boot handshake + menu\n");
    Rig r(2);
    CHECK(r.b->state == State::HOMING, "starts HOMING");
    CHECK(r.sim.homes_received == 1, "Pi sends CMD_HOME at boot");
    CHECK(r.until(State::MENU, 5000), "READY -> MENU");
    CHECK(r.b->menuIndex == 2, "menu starts on remembered mode, got %d", r.b->menuIndex);
    r.press(BTN_RIGHT); CHECK(r.b->menuIndex == 3, "right");
    r.press(BTN_RIGHT); CHECK(r.b->menuIndex == 0, "wraps right");
    r.press(BTN_LEFT);  CHECK(r.b->menuIndex == 3, "wraps left");
    r.press(BTN_CONFIRM); CHECK(r.b->inConfirm && r.b->state == State::MENU, "confirm screen");
    r.press(BTN_RIGHT);   CHECK(r.b->confirmIndex == 1, "Back highlighted");
    r.press(BTN_CONFIRM); CHECK(!r.b->inConfirm && r.b->state == State::MENU, "Back returns to menu");
}

static void test_pass_and_play_win() {
    printf("pass & play: RED wins on bottom row, selector skips full columns\n");
    Rig r; r.until(State::MENU, 5000); r.startMode(0);
    CHECK(r.b->state == State::HUMAN_TURN && r.b->mode == 0, "p&p human turn");
    // RED: 0,1,2,3 on bottom ; BLACK: 0,1,2 on row 1. Selector starts at column 0.
    int plan[7] = {0,0,1,1,2,2,3};
    int cur = 0;
    for (int col : plan) {
        while (r.b->selectedCol < col) r.press(BTN_RIGHT);
        while (r.b->selectedCol > col) r.press(BTN_LEFT);
        r.press(BTN_CONFIRM);
        CHECK(r.b->state == State::AWAITING_DROP, "awaiting drop");
        CHECK(r.sim.last_drop_col == col, "drop col %d got %d", col, r.sim.last_drop_col);
        if (!r.until(col == 3 ? State::GAME_OVER : State::HUMAN_TURN, 8000)) { CHECK(false, "no progress"); break; }
        cur++;
        if (col != 3) CHECK(r.b->selectedCol == 0, "selector resets to leftmost legal");
    }
    CHECK(r.b->winner == 0 && r.b->state == State::GAME_OVER, "RED wins, winner=%d", r.b->winner);
    CHECK(r.b->face == FACE_IDLE, "no win face in p&p");
    CHECK(r.sim.drops_received == 7, "exactly 7 drops, got %d", r.sim.drops_received);
}

static void test_full_column_skipped() {
    printf("full columns are skipped\n");
    Rig r; r.until(State::MENU, 5000); r.startMode(0);
    // fill column 0 with 6 alternating pieces (no wins): all in col 0
    for (int i = 0; i < 6; i++) {
        while (r.b->selectedCol > 0) r.press(BTN_LEFT);
        r.press(BTN_CONFIRM); r.until(State::HUMAN_TURN, 8000);
    }
    CHECK(!r.b->board.legal(0), "col 0 full");
    CHECK(r.b->selectedCol == 1, "selector lands on col 1, got %d", r.b->selectedCol);
    r.press(BTN_LEFT); CHECK(r.b->selectedCol == 1, "LEFT cannot select full col 0");
    CHECK(r.sim.last_selector == 1, "gantry told to go to 1");
}

static void test_ai_game() {
    printf("AI modes: AI (RED) moves first, human BLACK, game completes\n");
    for (int mode = 1; mode <= 3; mode++) {
        Rig r; r.until(State::MENU, 5000); r.startMode(mode);
        CHECK(r.b->state == State::AI_TURN || r.b->state == State::AWAITING_DROP, "mode %d: AI moves first", mode);
        CHECK(r.sim.drops_received >= 0 && r.b->mode == mode, "mode recorded");
        int guard = 0;
        while (r.b->state != State::GAME_OVER && r.b->state != State::FAULT && guard++ < 3000) {
            if (r.b->state == State::HUMAN_TURN) {
                // human plays the first legal column from the left
                r.press(BTN_CONFIRM);
            }
            r.run(50);
        }
        CHECK(r.b->state == State::GAME_OVER, "mode %d reached GAME_OVER (state %s)", mode, stateName(r.b->state));
        CHECK(r.sim.last_drop_color >= 0, "dropped");
        if (r.b->winner == 0) CHECK(r.b->face == FACE_WIN, "AI win face");
        if (r.b->winner == 1) CHECK(r.b->face == FACE_LOSS, "AI loss face");
        // No piece may ever be dropped twice: drops == pieces on board
        int pieces = 0; for (int c = 0; c < 7; c++) for (int rr = 0; rr < 6; rr++) if (r.b->board.at(c, rr) >= 0) pieces++;
        CHECK(pieces == r.sim.drops_received, "mode %d pieces %d == drops %d", mode, pieces, r.sim.drops_received);
        printf("  mode %d: winner=%d pieces=%d\n", mode, r.b->winner, pieces);
    }
}

static void test_faults() {
    printf("fault paths\n");
    { Rig r(2, true); r.until(State::FAULT, 10000);
      CHECK(r.b->state == State::FAULT && r.b->faultCode == ERR_STEPPER_STALL, "ERROR during HOMING is a fault"); }
    { Rig r; r.until(State::MENU, 5000); r.startMode(0); r.sim.drop_no_reply = true;
      r.press(BTN_CONFIRM); r.until(State::FAULT, 20000);
      CHECK(r.b->faultCode == FAULT_TIMEOUT, "timeout fault 0xFD, got 0x%02X", r.b->faultCode);
      CHECK(r.sim.drops_received == 1, "timeout must NOT resend the drop (got %d)", r.sim.drops_received); }
    { Rig r; r.until(State::MENU, 5000); r.startMode(0); r.sim.drop_wrong_col = true;
      r.press(BTN_CONFIRM); r.until(State::FAULT, 8000);
      CHECK(r.b->faultCode == FAULT_DESYNC, "mismatch fault 0xFF"); }
    { Rig r; r.until(State::MENU, 5000); r.startMode(0); r.sim.drop_error = true;
      r.press(BTN_CONFIRM); r.until(State::FAULT, 8000);
      CHECK(r.b->faultCode == ERR_STEPPER_STALL, "ESP32 error relayed"); 
      r.press(BTN_CONFIRM); r.press(BTN_LEFT); CHECK(r.b->state == State::FAULT, "FAULT is terminal"); }
}

static void test_restart_and_cancel() {
    printf("play again, and destroying Brain mid-search\n");
    { Rig r; r.until(State::MENU, 5000); r.startMode(0);
      int plan[7] = {0,0,1,1,2,2,3};
      for (int col : plan) { while (r.b->selectedCol < col) r.press(BTN_RIGHT); while (r.b->selectedCol > col) r.press(BTN_LEFT);
          r.press(BTN_CONFIRM); r.until(col == 3 ? State::GAME_OVER : State::HUMAN_TURN, 8000); }
      r.press(BTN_CONFIRM);
      CHECK(r.b->state == State::HOMING && r.sim.homes_received == 2, "confirm at game over re-homes");
      CHECK(r.until(State::MENU, 5000), "back to MENU"); }
    { Rig r; r.until(State::MENU, 5000); r.startMode(3);   // Hacker searching... destroy immediately
      CHECK(r.b->searching() || r.b->state != State::AI_TURN, "search running");
      /* ~Rig cancels + joins; must not hang or crash */ }
    CHECK(true, "destructor returned");
}

static void test_settings() {
    printf("settings file\n");
    std::string p = "/tmp/c4_settings_test/sub/settings.conf";
    Settings s; s.last_mode = 3; CHECK(s.save(p), "save creates dirs");
    Settings l; CHECK(l.load(p) && l.last_mode == 3, "round trip");
    FILE* f = fopen(p.c_str(), "w"); fputs("last_mode=banana\n", f); fclose(f);
    Settings bad; bad.last_mode = 2; CHECK(!bad.load(p) && bad.last_mode == 2, "corrupt file -> defaults");
    f = fopen(p.c_str(), "w"); fputs("last_mode=9\n", f); fclose(f);
    CHECK(!bad.load(p) && bad.last_mode == 2, "out of range -> defaults");
    CHECK(!bad.load("/tmp/c4_settings_test/missing.conf"), "missing file -> defaults");
}


static void test_reaction_timing() {
    printf("reaction to the human's move happens at the drop press, not after the gantry\n");
    Rig r; r.sim.drop_ms = 3000;                         // slow gantry, like the real one
    struct Ev { State st; std::string m; uint64_t t; };
    std::vector<Ev> events;
    r.b->log = [&](const std::string& m) { events.push_back({r.b->state, m, r.t}); };
    r.until(State::MENU, 5000);
    r.startMode(2);                                      // Pro
    CHECK(r.until(State::HUMAN_TURN, 20000), "first human turn");
    bool judgedOpening = false;
    for (auto& e : events) if (e.m.find("judged") != std::string::npos) judgedOpening = true;
    CHECK(!judgedOpening, "no reaction to R.O.G.E.R's own opening move");

    int judgedDuringDrop = 0, judgedLate = 0, quickReplies = 0, humanMoves = 0;
    for (int turn = 0; turn < 6 && r.b->state == State::HUMAN_TURN; turn++) {
        events.clear();
        r.press(BTN_CONFIRM);                            // human drops
        humanMoves++;
        for (int i = 0; i < 2000; i++) {
            r.run(10);
            if (r.b->state == State::HUMAN_TURN || r.b->state == State::GAME_OVER) break;
        }
        uint64_t aiTurnAt = 0, aiDropAt = 0;
        for (auto& e : events) {
            if (e.m.find("judged") != std::string::npos) { if (e.st == State::AWAITING_DROP) judgedDuringDrop++; else judgedLate++; }
            if (e.m == "-> AI_TURN" && !aiTurnAt) aiTurnAt = e.t;                 // the human's piece has landed
            if (e.m.find("SENT CMD_DROP") != std::string::npos && aiTurnAt && !aiDropAt) aiDropAt = e.t;
        }
        if (aiTurnAt && aiDropAt && aiDropAt - aiTurnAt <= 10) quickReplies++;
        if (r.b->state == State::GAME_OVER) break;
    }
    CHECK(judgedDuringDrop == humanMoves && judgedLate == 0, "every human move judged while its piece was still dropping (%d/%d, late %d)",
          judgedDuringDrop, humanMoves, judgedLate);
    CHECK(quickReplies >= humanMoves - 1, "R.O.G.E.R's reply sent right after the piece landed (%d of %d)", quickReplies, humanMoves);
}

int main() {
    test_boot_and_menu(); test_pass_and_play_win(); test_full_column_skipped();
    test_ai_game(); test_reaction_timing(); test_faults(); test_restart_and_cancel(); test_settings();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
