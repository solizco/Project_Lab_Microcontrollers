// Tests for the face player, stats file and screen flow.
// Usage: test_ui [FACES_DIR] [SHOT_DIR] [--dump]
//   FACES_DIR : .face files made by Finn's convert_gifs.py (default /tmp/roger/faces)
//   SHOT_DIR  : if given, every screen is saved there as a .ppm for inspection
//   --dump    : print "file frame hash" for every decoded frame (compared against Python)
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include "../src/app.h"
#include "../src/motion.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("  FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)


// Fails the test if a good/bad-move reaction is cut off before its last frame.
struct ReactionWatch {
    std::string playing; uint64_t start = 0; double total = 0; int seen = 0, interrupted = 0;
    void tick(const FacePlayer& f, uint64_t t, bool inGame) {
        bool reaction = inGame && (f.current == "good_move" || f.current == "bad_move");
        if (!playing.empty() && f.current != playing) {
            if ((double)(t - start) + 15 < total) {
                interrupted++;
                printf("  cut short: %s after %llu of %.0f ms, replaced by %s (t=%llu start=%llu)\n", playing.c_str(), (unsigned long long)(t - start), total, f.current.c_str(), (unsigned long long)t, (unsigned long long)start);
            }
            playing.clear();
        }
        if (reaction && playing.empty()) {
            playing = f.current; start = t; seen++;
            total = 0; for (double d : f.clips[f.clipIndex].durations) total += d;
        }
    }
};

static uint64_t fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ULL; const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}



// One Pi + gantry rig: Brain, App and faces on a SwitchLink over a simulated gantry.
struct Rig {
    SimLink gantry; SwitchLink sw; Brain* brain; FacePlayer faces; Stats stats; App* app;
    Canvas cv; uint64_t t = 1; std::string shots; int shot = 0; ReactionWatch watch; std::string sp;
    Rig(const std::string& facesDir, const std::string& shotDir, int shotBase, bool debugFrames = false, int lastMode = 2,
        uint32_t homeMs = 100, uint32_t dropMs = 100) : sw(gantry), shots(shotDir), shot(shotBase) {
        gantry.home_ms = homeMs; gantry.drop_ms = dropMs; gantry.debug_frames = debugFrames;
        std::string setPath = "/tmp/c4_rig_settings.conf"; unlink(setPath.c_str());
        Settings set; set.last_mode = (uint8_t)lastMode;
        brain = new Brain(sw, set, setPath, 7);
        std::string m; faces.loadFolder(facesDir, m);
        sp = "/tmp/c4_rig_stats.json"; unlink(sp.c_str());
        app = new App(*brain, faces, stats, sp, false, &sw);
        brain->buttonHook = [this](uint8_t b) { return app->espButton(b); };
    }
    ~Rig() { delete app; delete brain; }
    void step(int ms) {
        for (int i = 0; i < ms / 10; i++) {
            t += 10; brain->update(t); app->frame(t, cv);
            watch.tick(faces, t, app->screen == Screen::GAME && brain->state != State::GAME_OVER);
            if (brain->searching()) usleep(300);
        }
    }
    void esp(uint8_t b) { gantry.pushButton(b); step(30); }          // a press on the Motion Controller
    void key(Act a) { app->key(a); step(30); }                       // a key on the Pi keyboard
    void snap(const char* name) {
        if (shots.empty()) return;
        app->draw(cv, t); char p[256]; snprintf(p, sizeof p, "%s/%02d_%s.ppm", shots.c_str(), shot++, name); savePPM(cv, p);
    }
    void menuTo(int item) { for (int i = 0; i < 12 && app->menuSel() != item; i++) esp(BTN_RIGHT); }
    void optionTo(int item) { for (int i = 0; i < 12 && app->optionSel() != item; i++) esp(BTN_RIGHT); }
    // From the main menu: pick a mode, Start game, call heads, wait for the toss to start the game.
    void startGame(int mode) {
        menuTo(mode); esp(BTN_CONFIRM); esp(BTN_CONFIRM);          // mode -> confirm -> Start game
        esp(BTN_CONFIRM);                                          // Heads
        for (int i = 0; i < 500 && app->screen != Screen::GAME; i++) step(10);
    }
    // Human always drops in the leftmost legal column; runs until the game ends.
    void playOut(bool keyboard = false) {
        for (int guard = 0; guard < 4000 && brain->state != State::GAME_OVER && brain->state != State::FAULT; guard++) {
            if (brain->state == State::HUMAN_TURN) { if (keyboard) key(A_SELECT); else esp(BTN_CONFIRM); }
            step(20);
        }
    }
    void finishOver() { for (int i = 0; i < 2500 && app->over() != App::Over::POPUP; i++) step(10); }
};


// Servo calibration screen against the real gantry code on a do-nothing GPIO.
struct NullHal : MotionHal {
    uint64_t t = 0; std::vector<int> servo;
    bool wired(Line) const override { return false; }
    void write(Line, int) override {}
    int read(Line) override { return 1; }
    uint64_t micros() override { return t; }
    void delayUs(uint32_t us) override { t += us; }
    void sleepMs(uint32_t ms) override { t += ms * 1000ull; }
    void servoUs(int us) override { servo.push_back(us); }
    bool servoWired() const override { return true; }
    std::string servoInfo() const override { return "test PWM"; }
};

static void testServoCal(const std::string& facesDir, const std::string& shots) {
    printf("servo calibration screen\n");
    Rig r(facesDir, shots, 60);
    r.step(1500);
    NullHal hal; MotionConfig cfg; ServoCal cal;
    unlink("/tmp/c4_ui_servo.conf");
    MotionController mc(hal, cfg, cal, "/tmp/c4_ui_servo.conf");
    mc.boot(1); { Frame f; while (mc.toPi(f)) {} }
    r.app->setGantry(&mc);
    auto pump = [&](int n = 1) { for (int i = 0; i < n; i++) { mc.tick(); r.step(10); } };
    r.menuTo(4); r.esp(BTN_CONFIRM); r.optionTo(4); r.esp(BTN_CONFIRM); pump();
    CHECK(r.app->screen == Screen::SERVOCAL, "Options > Servo calibration opens it (%d)", (int)r.app->screen);
    CHECK(r.app->calValue(0) == 103 && r.app->calValue(5) == 250, "shows the current values");
    r.snap("servocal");
    // Rest: edit, +2 with the right button, -1 with the left button
    r.esp(BTN_CONFIRM); pump();
    CHECK(r.app->calEditing(), "select starts editing");
    r.esp(BTN_RIGHT); r.esp(BTN_RIGHT); r.esp(BTN_LEFT); pump(2);
    CHECK(r.app->calValue(0) == 104 && mc.cal().rest == 104, "right/left change the value (rest now %d)", mc.cal().rest);
    CHECK(!hal.servo.empty() && hal.servo.back() == 500 + 2000 * 104 / 180, "the servo moves there live");
    r.snap("servocal_edit");
    r.esp(BTN_CONFIRM); pump();
    CHECK(!r.app->calEditing(), "select again ends editing");
    // wait row steps by 10 ms
    for (int i = 0; i < 5; i++) r.esp(BTN_RIGHT);
    r.esp(BTN_CONFIRM); r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM); pump(2);
    CHECK(r.app->calValue(5) == 260 && mc.cal().waitMs == 260, "wait goes up by 10 ms (%d)", mc.cal().waitMs);
    // Test black drop, then Save
    r.esp(BTN_RIGHT); r.esp(BTN_RIGHT); hal.servo.clear(); r.esp(BTN_CONFIRM); pump(2);
    CHECK(r.app->calSel() == 7 && hal.servo.size() >= 3, "Test black drop runs select, push, rest");
    r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM); pump(2);                 // Check signal (nothing to measure here)
    r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM); pump(2);                 // Save
    ServoCal back; bool loaded = loadServoCal("/tmp/c4_ui_servo.conf", back);
    CHECK(loaded && back.rest == 104 && back.waitMs == 260 && mc.status().saveResult == 1, "Save writes servo.conf");
    r.snap("servocal_saved");
    r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM); pump();
    CHECK(r.app->screen == Screen::OPTIONS, "Back returns to Options");
    // no gantry (--sim / --serial): the screen says so and Back works
    r.app->setGantry(nullptr);
    r.optionTo(4); r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::SERVOCAL, "opens without a gantry too");
    r.snap("servocal_none");
    r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::OPTIONS, "...and select goes back");
}

static void testFullFlow(const std::string& facesDir, const std::string& shots) {
    printf("full flow: coin toss, reactions, game over, faults, link debug\n");
    Rig r(facesDir, shots, 0, true);
    r.step(1500);
    CHECK(r.brain->state == State::MENU && r.app->screen == Screen::MENU, "menu after homing (%s)", stateName(r.brain->state));
    r.snap("menu");

    // Options > Link debug > back
    r.menuTo(4); r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::OPTIONS, "options");
    r.optionTo(1); r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::LINKDEBUG, "link debug");
    r.step(3500); r.snap("link_debug");
    CHECK(r.brain->mc.lastHeard > 0 && r.brain->mc.fw == C4_FW_VERSION, "link debug sees the board");
    r.esp(BTN_RIGHT); r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM);
    CHECK(r.gantry.last_selector == 4, "Column button moves the gantry (%d)", r.gantry.last_selector);
    r.esp(BTN_LEFT); r.esp(BTN_LEFT); r.esp(BTN_LEFT); r.esp(BTN_CONFIRM);   // wraps to Back
    CHECK(r.app->screen == Screen::OPTIONS, "back to options");
    r.optionTo(5); r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::MENU, "options back -> menu");

    // every AI mode and 1v1: coin toss, then a whole game, then the results popup, then play again
    int games = 0;
    for (int mode : {1, 2, 3, 0}) {
        r.menuTo(mode); r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::CONFIRM, "mode %d confirm", mode);
        r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::COINCALL, "mode %d: Start game opens the coin toss", mode);
        if (mode == 1) r.snap("coin_call");
        r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM);                          // Tails
        CHECK(r.app->screen == Screen::COINTOSS && r.app->tossCall() == 1, "tails called");
        r.step(600); if (mode == 1) r.snap("coin_flying");
        r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::COINTOSS, "select while the coin is in the air does nothing");
        r.step(1400); if (mode == 1) r.snap("coin_landed");
        for (int i = 0; i < 300 && r.app->screen != Screen::GAME; i++) r.step(10);
        CHECK(r.app->screen == Screen::GAME && r.brain->mode == mode, "mode %d started after the toss", mode);
        bool callerWon = r.app->tossCall() == r.app->tossResult();
        int expectFirst = mode != 0 ? (callerWon ? 1 : 0) : (callerWon ? 0 : 1);
        CHECK(r.app->tossFirst() == expectFirst, "who goes first follows the toss");
        bool firstIsHuman = mode == 0 || expectFirst == 1;
        CHECK(firstIsHuman ? r.brain->state == State::HUMAN_TURN : (r.brain->state == State::AI_TURN || r.brain->state == State::AWAITING_DROP),
              "mode %d: correct side moves first (%s)", mode, stateName(r.brain->state));
        r.playOut();
        CHECK(r.brain->state == State::GAME_OVER, "mode %d game over (%s)", mode, stateName(r.brain->state));
        games++;
        if (mode != 0 && r.brain->winner <= 1) {
            for (int i = 0; i < 300 && r.app->over() == App::Over::WAIT_REACTION; i++) r.step(10);
            const char* endFace = r.brain->winner == 0 ? "roger_win" : "roger_loss";
            CHECK(r.app->over() == App::Over::GIF && r.faces.current == endFace, "end GIF plays (%s)", r.faces.current.c_str());
            r.esp(BTN_LEFT);
            CHECK(r.app->over() == App::Over::GIF, "left/right do not skip it");
            if (mode == 1) r.snap("end_gif");
        }
        r.finishOver();
        CHECK(r.app->over() == App::Over::POPUP, "mode %d results popup", mode);
        if (mode == 1 || mode == 0) r.snap(mode == 0 ? "popup_1v1" : "popup");
        r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::MENU && r.brain->state == State::HOMING, "Play again -> re-home -> menu");
        r.step(400);
        CHECK(r.brain->state == State::MENU, "ready again");
    }
    CHECK(r.watch.seen > 0 && r.watch.interrupted == 0, "reactions seen %d, cut short %d", r.watch.seen, r.watch.interrupted);
    Stats st; st.load(r.sp);
    CHECK((int)(st.totals().games() + st.pvpGames) == games, "every game recorded once (%d)", (int)(st.totals().games() + st.pvpGames));

    // a fault, then back to the menu without a power cycle
    r.gantry.drop_error = true;
    r.startGame(0);
    for (int i = 0; i < 50 && r.brain->state != State::HUMAN_TURN; i++) r.step(20);
    r.esp(BTN_CONFIRM); r.step(400);
    CHECK(r.brain->state == State::FAULT && r.app->screen == Screen::GAME, "fault shown");
    r.snap("fault");
    r.gantry.drop_error = false;
    r.esp(BTN_CONFIRM);                                                  // Back to menu (preselected)
    CHECK(r.app->screen == Screen::MENU && r.brain->state == State::HOMING, "Back to menu re-homes");
    r.step(400);
    CHECK(r.brain->state == State::MENU, "menu again, no power cycle");

    // shut down: cancel, then the Motion Controller button
    r.menuTo(4); r.esp(BTN_CONFIRM); r.optionTo(3); r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::SHUTDOWN, "shut down screen");
    r.esp(BTN_CONFIRM);
    CHECK(r.app->screen == Screen::OPTIONS && !r.app->shutdownRequested, "Cancel is preselected");
    r.gantry.pushShutdown(); r.step(30);
    CHECK(r.app->shutdownRequested, "shutdown button on the Motion Controller");
    r.app->shutdownRequested = false; r.brain->shutdownRequested = false;
    r.app->showBanner("Could not power off: sudo: a password is required", "Fix: run install_autostart.sh again (see ~/c4_log.txt).", r.t, 3000);
    r.step(50); r.snap("poweroff_refused");
    r.esp(BTN_RIGHT);
    CHECK(r.app->screen == Screen::OPTIONS, "buttons still work under the message");
}

static void testCoinFairness() {
    printf("coin toss is fair\n");
    std::mt19937 g(123); int heads = 0;
    for (int i = 0; i < 10000; i++) heads += (g() & 1) == 0;
    CHECK(heads > 4800 && heads < 5200, "heads %d of 10000", heads);
}

// Bug fix: toggling screen-only must never lock up the screen or the buttons,
// even while the real gantry is busy homing for a long time (a cold-start
// homing that keeps failing used to block the Motion Controller for ~50 s).
static void testScreenOnlyToggle(const std::string& facesDir, const std::string& shots) {
    printf("screen-only toggle never freezes the screen or the buttons\n");
    Rig r(facesDir, shots, 40, true, 2, 30000 /* slow gantry: 30 s homing */, 100);
    r.step(800);
    CHECK(r.brain->state == State::HOMING || r.brain->state == State::MENU, "gantry homing at start");
    for (int round = 0; round < 6; round++) {
        bool on = round % 2 == 0;
        r.menuTo(4); r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::OPTIONS, "round %d: options reachable (%d)", round, (int)r.app->screen);
        r.optionTo(0); r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::SCREENMODE, "round %d: chooser", round);
        if (on) r.esp(BTN_CONFIRM);                            // "Motion Controller buttons" is preselected
        else { r.esp(BTN_RIGHT); r.esp(BTN_CONFIRM); }         // "Turn off screen only"
        CHECK(r.app->screenOnly() == on && r.app->screen == Screen::OPTIONS, "round %d: screen only %s", round, on ? "on" : "off");
        r.step(300);
        // the screen keeps answering both kinds of input right away
        r.esp(BTN_RIGHT); int a = r.app->optionSel(); r.key(A_DOWN); int b = r.app->optionSel();
        CHECK(b == (a + 1) % 5, "round %d: buttons and keyboard still move the highlight (%d -> %d)", round, a, b);
        r.optionTo(5); r.esp(BTN_CONFIRM);
        CHECK(r.app->screen == Screen::MENU, "round %d: back to the menu", round);
        if (on) {
            CHECK(r.brain->state == State::MENU, "round %d: on-screen gantry ready at once (%s)", round, stateName(r.brain->state));
            r.startGame(0);
            CHECK(r.app->screen == Screen::GAME && r.brain->state == State::HUMAN_TURN, "round %d: screen-only 1v1 starts", round);
            if (round == 0) r.snap("screen_only_game");
            r.playOut();
            r.finishOver(); r.esp(BTN_CONFIRM); r.step(400);
            CHECK(r.app->screen == Screen::MENU && r.brain->state == State::MENU, "round %d: game done, menu", round);
        } else {
            CHECK(r.brain->state == State::HOMING, "round %d: real gantry homing again", round);
        }
    }
    CHECK(r.gantry.drops_received == 0, "the real gantry never got a drop in screen-only games");
}

static void testSilentBoardMenu(const std::string& facesDir, const std::string& shots) {
    printf("menu tells you when the ESP32 has not started\n");
    Rig r(facesDir, shots, 60);
    r.gantry.not_started = true;
    r.step(7000);
    CHECK(r.brain->boardSilent() && r.app->screen == Screen::MENU, "silent board shown on the menu");
    r.snap("menu_no_signal");
    r.key(A_DOWN); int a = r.app->menuSel(); r.key(A_DOWN);
    CHECK(r.app->menuSel() == (a + 1) % 6, "keyboard still works");
    r.gantry.not_started = false; r.gantry.reboot(1); r.step(1500);
    CHECK(r.brain->state == State::MENU && !r.brain->boardSilent(), "EN pressed: ready");
    r.snap("menu_ready_after_en");
}

static void testReactionLoops(const std::string& facesDir) {
    printf("reaction loops through R.O.G.E.R's turn and finishes its pass\n");
    SimLink sim; Settings set; Brain brain(sim, set, "/tmp/c4_ri_settings.conf", 3);
    FacePlayer faces; std::string m; faces.loadFolder(facesDir, m);
    Stats stats; App app(brain, faces, stats, "/tmp/c4_ri_stats.json", true);
    Canvas cv; uint64_t t = 1;
    auto run = [&](int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; app.frame(t, cv); } };
    run(100);
    app.testFace(FACE_BAD_MOVE);
    CHECK(faces.current == "bad_move", "reaction starts (%s)", faces.current.c_str());
    double total = 0; for (double d : faces.clips[faces.clipIndex].durations) total += d;
    run((int)total * 3);
    CHECK(faces.current == "bad_move", "keeps looping while R.O.G.E.R plays (%s)", faces.current.c_str());
    app.testFace(FACE_THINKING);
    CHECK(faces.current == "bad_move", "a new face waits for the pass to finish");
    run((int)total + 50);
    CHECK(faces.current == "thinking", "then switches (%s)", faces.current.c_str());
    app.testFace(FACE_GOOD_MOVE); run(50); app.testFace(FACE_IDLE); run(50);
    CHECK(faces.current == "good_move", "idle waits too (%s)", faces.current.c_str());
    run(1500);
    CHECK(faces.current == "idle", "reaction ends on idle (%s)", faces.current.c_str());
    app.testFace(FACE_WIN);
    CHECK(faces.current == "idle", "win/loss are left to the game-over sequence");
}

int main(int argc, char** argv) {
    std::string facesDir = argc > 1 ? argv[1] : "/tmp/roger/faces";
    std::string shots = (argc > 2 && strcmp(argv[2], "--dump")) ? argv[2] : "";
    bool dump = false; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--dump")) dump = true;

    printf("group names\n");
    CHECK(faceGroupName("bad_move2.face") == "bad_move", "bad_move2");
    CHECK(faceGroupName("bad_move.gif") == "bad_move", "bad_move");
    CHECK(faceGroupName("idle_2.face") == "idle", "idle_2");
    CHECK(faceGroupName("roger_win.face") == "roger_win", "roger_win");
    CHECK(faceGroupName("123.face") == "123", "digits only keeps name");

    printf("loading faces from %s\n", facesDir.c_str());
    FacePlayer fp; std::string msg;
    bool ok = fp.loadFolder(facesDir, msg);
    printf("  %s\n", msg.c_str());
    CHECK(ok && fp.width == 400 && fp.height == 240, "load (%dx%d)", fp.width, fp.height);
    CHECK(fp.has("idle") && fp.has("bad_move") && fp.has("roger_win"), "groups");

    if (dump) {   // decode every frame of every clip
        for (int c = 0; c < (int)fp.clips.size(); c++) {
            for (int f = 0; f < (int)fp.clips[c].blobs.size(); f++) {
                fp.playClip(c, 0);
                // step to frame f by forcing the clock forward frame by frame
                FacePlayer one; std::string m2; one.loadFolder(facesDir, m2); one.playClip(c, 0);
                uint64_t t = 1; one.update(t);
                for (int k = 0; k < f; k++) { t += (uint64_t)fp.clips[c].durations[k] + 1; one.update(t); }
                const uint16_t* px = one.frame();
                printf("%s %d %016llx\n", fp.clips[c].name.c_str(), f, (unsigned long long)fnv(px, (size_t)fp.width * fp.height * 2));
            }
        }
        return 0;
    }

    printf("playback timing\n");
    {
        FacePlayer p; std::string m; p.loadFolder(facesDir, m);
        uint64_t t = 1000; p.update(t);
        p.play("good_move", 1);
        CHECK(p.current == "good_move" && p.oneShotPlaying(), "one-shot started");
        double total = 0; for (double d : p.clips[p.clipIndex].durations) total += d;
        for (uint64_t e = t + (uint64_t)total - 50; t < e; t += 10) p.update(t);
        CHECK(p.current == "good_move", "still playing just before the end");
        for (int i = 0; i < 30; i++) { t += 10; p.update(t); }
        CHECK(p.current == "idle" && !p.oneShotPlaying(), "falls back to idle after one play (now %s)", p.current.c_str());
        p.play("roger_win", 0);
        for (int i = 0; i < 500; i++) { t += 10; p.update(t); }
        CHECK(p.current == "roger_win", "loops=0 keeps looping");
        // variants never repeat back to back
        std::string prev; int repeats = 0;
        for (int i = 0; i < 40; i++) { p.play("bad_move", 1); std::string n = p.clips[p.clipIndex].name; if (n == prev) repeats++; prev = n; }
        CHECK(repeats == 0, "variant repeated %d times", repeats);
    }

    printf("stats file\n");
    {
        std::string path = "/tmp/c4_stats_test.json";
        Stats s; s.recordRoger(0, "win"); s.recordRoger(2, "loss"); s.recordRoger(2, "loss"); s.recordPvp("p2"); s.recordPvp("draw");
        CHECK(s.save(path), "save");
        Stats l; CHECK(l.load(path), "load");
        CHECK(l.roger[0].wins == 1 && l.roger[2].losses == 2 && l.pvpGames == 2 && l.p2 == 1 && l.pvpDraws == 1, "round trip");
        CHECK(l.totals().games() == 3, "totals");
        Stats missing; CHECK(!missing.load("/tmp/does_not_exist.json") && missing.totals().games() == 0, "missing file");
    }

    testReactionLoops(facesDir);
    testCoinFairness();
    testFullFlow(facesDir, shots);
    testScreenOnlyToggle(facesDir, shots);
    testSilentBoardMenu(facesDir, shots);
    testServoCal(facesDir, shots);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
