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
    { Rig r(2, true); r.until(State::FAULT, 15000);
      CHECK(r.b->state == State::FAULT && r.b->faultCode == ERR_STEPPER_STALL, "ERROR during HOMING is a fault");
      CHECK(r.sim.resets_received == 1, "...after one automatic restart of the board (%d)", r.sim.resets_received); }
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
    Settings d; CHECK(d.screen_input == 1, "screen-only default: Motion Controller buttons");
    f = fopen(p.c_str(), "w"); fputs("last_mode=1\nscreen_input=keyboard\n", f); fclose(f);
    Settings v1; CHECK(v1.load(p) && v1.screen_input == 1, "old (version 1) file: keyboard default ignored");
    Settings k; k.screen_input = 0; k.save(p); Settings v2; CHECK(v2.load(p) && v2.screen_input == 0, "version 2 keeps a chosen keyboard");
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
            if (e.m.find("Pi > MC: DROP") != std::string::npos && aiTurnAt && !aiDropAt) aiDropAt = e.t;
        }
        if (aiTurnAt && aiDropAt && aiDropAt - aiTurnAt <= 10) quickReplies++;
        if (r.b->state == State::GAME_OVER) break;
    }
    CHECK(judgedDuringDrop == humanMoves && judgedLate == 0, "every human move judged while its piece was still dropping (%d/%d, late %d)",
          judgedDuringDrop, humanMoves, judgedLate);
    CHECK(quickReplies >= humanMoves - 1, "R.O.G.E.R's reply sent right after the piece landed (%d of %d)", quickReplies, humanMoves);
}

static void test_link_debug() {
    printf("link debugging and fault recovery\n");
    auto run = [](Brain& b, uint64_t& t, uint64_t ms) { for (uint64_t e = t + ms; t < e; t += 10) { b.update(t); if (b.searching()) usleep(1000); } };
    { // Pi -> MC wire broken: the board keeps beaconing READY
      SimLink sim; sim.deaf = true; Settings s; Brain b(sim, s, "/tmp/c4_ld.conf", 1); uint64_t t = 0;
      run(b, t, 1000);
      CHECK(b.state == State::MENU, "deaf board: first beacon still reaches the menu (%s)", stateName(b.state));
      run(b, t, 8000);
      CHECK(b.state == State::FAULT && b.faultCode == FAULT_DEAF, "deaf board -> FAULT 0xFC (%s 0x%02X)", stateName(b.state), b.faultCode);
      bool logged = false; for (auto& e : b.linkLog) logged |= e.fromMc && e.text == "READY";
      CHECK(logged && !b.linkLog.empty(), "traffic is in the link log");
      b.resetToMenu(); CHECK(b.state == State::HOMING, "Back to menu re-homes");
      run(b, t, 9000);
      CHECK(b.state == State::FAULT && b.faultCode == FAULT_DEAF, "still deaf -> faults again, no power cycle needed");
      sim.deaf = false;                                  // wire fixed
      b.resetToMenu(); run(b, t, 10000);
      CHECK(b.state == State::MENU, "wire fixed: Back to menu works and stays in MENU (%s)", stateName(b.state));
    }
    { // healthy v2 firmware
      SimLink sim; sim.debug_frames = true; Settings s; Brain b(sim, s, "/tmp/c4_ld.conf", 1); uint64_t t = 0;
      run(b, t, 8000);
      CHECK(b.state == State::MENU, "healthy board stays in MENU");
      CHECK(b.mc.lastHeard > 0 && b.mc.fw == C4_FW_VERSION && b.mc.statusKnown && b.mc.homed, "status known: hears the Pi, fw v%d, homed", b.mc.fw);
      int drops = sim.drops_received;
      CHECK(b.debugMove(5) && sim.last_selector == 5, "debug move sends the selector");
      CHECK(b.debugDrop(2, 0) && sim.drops_received == drops + 1, "debug drop reaches the board");
      run(b, t, 3000);
      CHECK(b.state == State::MENU && b.board.state().heights[2] == 0, "debug drop leaves the game board and state alone");
      // board restarts mid-game (brownout)
      b.startGame(0, 0); b.onButton(BTN_CONFIRM);
      CHECK(!b.debugMove(1), "debug tools refused mid-game");
      sim.reboot(9); run(b, t, 50);
      CHECK(b.state == State::FAULT && b.faultCode == FAULT_MC_RESET && b.mc.resetReason == 9, "restart mid-game -> FAULT 0xFB, reason brownout");
      b.restartMotionController();
      CHECK(b.state == State::HOMING, "Restart gantry -> HOMING");
      run(b, t, 10000);
      CHECK(b.state == State::MENU && b.mc.resetReason == 3, "board restarted, back in MENU (%s, reason %d)", stateName(b.state), b.mc.resetReason);
      CHECK(b.startGame(2, 1), "a new game starts after recovery");
    }
    { // old firmware board restarts while the Pi sits in the menu: no false alarm
      SimLink sim; Settings s; Brain b(sim, s, "/tmp/c4_ld.conf", 1); uint64_t t = 0;
      run(b, t, 3000); sim.reboot(); run(b, t, 12000);
      CHECK(b.state == State::MENU, "beacon after a menu-time restart is stopped by a ping (%s)", stateName(b.state));
    }
    { // drop timeout, then recover from the fault screen
      SimLink sim; Settings s; Brain b(sim, s, "/tmp/c4_ld.conf", 1); uint64_t t = 0;
      run(b, t, 2000); sim.drop_no_reply = true;
      b.startGame(0, 0); b.onButton(BTN_CONFIRM); run(b, t, 16000);
      CHECK(b.state == State::FAULT && b.faultCode == FAULT_TIMEOUT, "drop timeout -> 0xFD");
      sim.drop_no_reply = false; b.resetToMenu(); run(b, t, 2000);
      CHECK(b.state == State::MENU && b.winner == -1 && b.faultCode == 0, "Back to menu clears the fault");
      CHECK(b.startGame(1, 1) && b.state == State::HUMAN_TURN && b.board.state().heights[3] == 0, "fresh game, empty board");
    }
}



static void test_human_first() {
    printf("coin toss: the human (or Player Two) can move first\n");
    for (int mode = 0; mode <= 3; mode++) {
        Rig r; r.until(State::MENU, 5000);
        CHECK(r.b->startGame(mode, 1), "mode %d started", mode);
        CHECK(r.b->state == State::HUMAN_TURN && r.b->board.toMove() == 1, "mode %d: BLACK (human / Player Two) moves first", mode);
        r.press(BTN_CONFIRM);
        CHECK(r.sim.last_drop_color == 1, "first piece dropped is BLACK");
        bool next = mode == 0 ? r.until(State::HUMAN_TURN, 8000) : r.until(State::AWAITING_DROP, 30000) || r.b->state == State::HUMAN_TURN;
        CHECK(next, "game continues (%s)", stateName(r.b->state));
        if (mode != 0) { r.until(State::HUMAN_TURN, 30000); CHECK(r.sim.last_drop_color == 0, "R.O.G.E.R plays RED"); }
    }
}

static void test_shutdown_button() {
    printf("shutdown button on the Motion Controller\n");
    Rig r; r.until(State::MENU, 5000);
    CHECK(!r.b->shutdownRequested, "not requested at start");
    r.sim.pushShutdown(); r.run(20);
    CHECK(r.b->shutdownRequested, "CMD_SHUTDOWN sets shutdownRequested in MENU");
    Rig g; g.until(State::MENU, 5000); g.startMode(0); g.sim.pushShutdown(); g.run(20);
    CHECK(g.b->shutdownRequested, "...and mid-game");
}

static void test_rezero_every_turn() {
    printf("1v1 and vs R.O.G.E.R: every turn ends with a drop (the board re-zeros after each one), then the gantry goes out\n");
    for (int mode : {0, 2}) {
        Rig r; r.until(State::MENU, 5000); r.startMode(mode);
        int humanTurns = 0, guard = 0;
        while (r.b->state != State::GAME_OVER && r.b->state != State::FAULT && guard++ < 2000) {
            if (r.b->state == State::HUMAN_TURN) {
                humanTurns++;
                CHECK(r.sim.last_selector == r.b->selectedCol, "mode %d: turn starts by sending the gantry to the selected column", mode);
                r.press(BTN_CONFIRM);
            }
            r.run(50);
        }
        int pieces = 0; for (int c = 0; c < 7; c++) for (int rr = 0; rr < 6; rr++) if (r.b->board.at(c, rr) >= 0) pieces++;
        CHECK(r.b->state == State::GAME_OVER && pieces == r.sim.drops_received, "mode %d: one drop (and one re-zero on the board) per turn: %d", mode, pieces);
    }
}

static void test_cold_start_restart() {
    printf("bug fix: board that cannot home after a cold start is restarted automatically (like pressing EN)\n");
    SimLink sim; sim.stall_until_reset = true; sim.debug_frames = true;
    Settings s; Brain b(sim, s, "/tmp/c4_cs.conf", 1); uint64_t t = 0;
    std::string log; b.log = [&](const std::string& m) { log += m + "\n"; };
    for (int i = 0; i < 1500 && b.state != State::MENU; i++) { b.update(t); t += 10; }
    CHECK(b.state == State::MENU, "reached the menu without anyone pressing EN (%s 0x%02X)", stateName(b.state), b.faultCode);
    CHECK(sim.resets_received == 1, "exactly one automatic restart (%d)", sim.resets_received);
    CHECK(log.find("restarting the Motion Controller automatically") != std::string::npos, "logged why");
    // a second failure later (after a good home) gets its own automatic restart
    sim.stall_until_reset = true; b.rehome();
    for (int i = 0; i < 1500 && !(b.state == State::MENU && sim.resets_received == 2); i++) { b.update(t); t += 10; }
    CHECK(b.state == State::MENU && sim.resets_received == 2, "re-home failure also recovers (%s, %d resets)", stateName(b.state), sim.resets_received);
}

static void test_silent_board() {
    printf("ESP32 that never started: detected, said plainly, reset over the TX wire when the resistor is fitted\n");
    auto run = [](Brain& b, uint64_t& t, uint64_t ms) { for (uint64_t e = t + ms; t < e; t += 10) b.update(t); };
    { // no resistor: nothing can be done from the Pi, but it knows and says so
      SimLink sim; sim.not_started = true; Settings s; Brain b(sim, s, "/tmp/c4_sb.conf", 1); uint64_t t = 1;
      std::string log; b.log = [&](const std::string& m) { log += m + "\n"; };
      run(b, t, 5000);
      CHECK(!b.boardSilent() && sim.breaks_received == 0, "not called silent in the first seconds");
      run(b, t, 2000);
      CHECK(b.boardSilent() && sim.breaks_received == 1, "silent after 6 s: first reset attempt (%d)", sim.breaks_received);
      run(b, t, 60000);
      CHECK(b.state == State::HOMING && b.boardSilent(), "still waiting, not a fault");
      CHECK(sim.breaks_received == SILENT_BREAK_MAX, "reset attempts are capped (%d)", sim.breaks_received);
      CHECK(log.find("no signal from the Motion Controller") != std::string::npos, "logged");
      // someone presses EN: the board boots, homes and beacons
      sim.not_started = false; sim.reboot(1); run(b, t, 3000);
      CHECK(b.state == State::MENU && !b.boardSilent(), "EN pressed: menu (%s)", stateName(b.state));
    }
    { // resistor fitted: the Pi starts the board by itself
      SimLink sim; sim.not_started = true; sim.break_wired = true; sim.debug_frames = true;
      Settings s; Brain b(sim, s, "/tmp/c4_sb.conf", 1); uint64_t t = 1;
      run(b, t, 9000);
      CHECK(sim.breaks_received == 1 && b.state == State::MENU, "one reset over the TX wire and the menu is up (%s, %d)", stateName(b.state), sim.breaks_received);
      run(b, t, 20000);
      CHECK(sim.breaks_received == 1 && b.state == State::MENU, "no more resets once the board talks");
    }
    { // a healthy board is never reset
      SimLink sim; sim.debug_frames = true; sim.home_ms = 12000; Settings s; Brain b(sim, s, "/tmp/c4_sb.conf", 1); uint64_t t = 1;
      run(b, t, 11000);
      CHECK(sim.breaks_received == 0 && !b.boardSilent(), "slow but talking board: left alone");
    }
    { // homing failed, then the firmware homes by itself: back to the menu without a button
      SimLink sim; sim.fail_home = true; Settings s; Brain b(sim, s, "/tmp/c4_sb.conf", 1); uint64_t t = 1;
      run(b, t, 15000);
      CHECK(b.state == State::FAULT && b.faultCode == ERR_STEPPER_STALL, "homing fault first");
      sim.fail_home = false; sim.pushReady(); run(b, t, 100);
      CHECK(b.state == State::MENU && b.faultCode == 0, "READY after a homing fault clears it (%s)", stateName(b.state));
      CHECK(b.startGame(0, 0), "and a game can start");
      // a mid-game fault is NOT cleared by a stray READY
      sim.drop_error = true; b.onButton(BTN_CONFIRM); run(b, t, 3000);
      CHECK(b.state == State::FAULT, "mid-game fault");
      sim.pushReady(); run(b, t, 100);
      CHECK(b.state == State::FAULT, "mid-game fault stays until the player chooses");
    }
}

int main() {
    test_boot_and_menu(); test_pass_and_play_win(); test_full_column_skipped();
    test_ai_game(); test_reaction_timing(); test_faults(); test_restart_and_cancel(); test_settings();
    test_link_debug(); test_human_first(); test_shutdown_button(); test_rezero_every_turn(); test_cold_start_restart(); test_silent_board();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
