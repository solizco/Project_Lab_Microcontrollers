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


// Screen-only mode: games run on the display, the gantry never receives a
// command, and the ESP32's buttons still drive everything.
static void testScreenOnly(const std::string& facesDir, const std::string& shots) {
    printf("screen-only mode\n");
    SimLink gantry; gantry.home_ms = 100; gantry.drop_ms = 100;
    SwitchLink sw(gantry);
    std::string setPath = "/tmp/c4_so_settings.conf"; unlink(setPath.c_str());
    Settings set; set.last_mode = 2;
    Brain brain(sw, set, setPath, 11);
    FacePlayer faces; std::string m; faces.loadFolder(facesDir, m);
    Stats stats; std::string sp = "/tmp/c4_so_stats.json"; unlink(sp.c_str());
    App app(brain, faces, stats, sp, false, &sw);
    brain.buttonHook = [&app](uint8_t b) { return app.espButton(b); };
    Canvas cv; uint64_t t = 1; int shot = 50;
    ReactionWatch watch;
    auto step = [&](int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; brain.update(t); app.frame(t, cv); watch.tick(faces, t, app.screen == Screen::GAME); if (brain.searching()) usleep(500); } };
    auto esp = [&](uint8_t b) { gantry.pushButton(b); step(30); };    // a press on the real ESP32
    auto snap = [&](const char* name) { if (shots.empty()) return; app.draw(cv, t); char p[256]; snprintf(p, sizeof p, "%s/%02d_%s.ppm", shots.c_str(), shot++, name); savePPM(cv, p); };

    step(1500);
    CHECK(brain.state == State::MENU && !app.screenOnly(), "starts with the gantry");
    int homesBefore = gantry.homes_received;
    // Pro is highlighted; two RIGHTs = Options; select; first item = Screen only
    esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_CONFIRM);
    CHECK(app.screen == Screen::OPTIONS, "options from ESP32 buttons");
    esp(BTN_CONFIRM);
    CHECK(app.screen == Screen::SCREENMODE, "screen only opens the controls chooser");
    snap("screenmode_chooser_off");
    esp(BTN_RIGHT); esp(BTN_CONFIRM);                       // "Motion Controller buttons"
    CHECK(app.screenOnly() && app.screenInput() == 1 && app.screen == Screen::OPTIONS, "screen only on, buttons");
    snap("options_screen_only_on");
    step(400);
    CHECK(brain.state == State::MENU, "on-screen gantry ready (%s)", stateName(brain.state));
    Settings reread; reread.load(setPath);
    CHECK(reread.screen_only && reread.screen_input == 1, "screen only + buttons remembered in the settings file");
    // Back (4th item): three RIGHTs from "Screen only", select. Menu is still on Options (4): two LEFTs = Pro.
    esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_CONFIRM);
    CHECK(app.screen == Screen::MENU, "back to menu");
    snap("menu_screen_only");
    esp(BTN_LEFT); esp(BTN_LEFT); esp(BTN_CONFIRM); esp(BTN_CONFIRM);
    CHECK(app.screen == Screen::GAME && brain.mode == 2, "Pro started (mode %d)", brain.mode);

    bool snappedHuman = false, snappedFall = false;
    for (int i = 0; i < 3000 && brain.state != State::GAME_OVER; i++) {
        step(10);
        if (brain.state == State::AWAITING_DROP && !snappedFall && t > 0) {
            step(150); if (brain.state == State::AWAITING_DROP) { snap("so_falling"); snappedFall = true; }
        }
        if (brain.state == State::HUMAN_TURN) {
            if (!snappedHuman && brain.board.state().heights[3] >= 1) {
                esp(BTN_RIGHT); esp(BTN_RIGHT); snap("so_human_turn"); snappedHuman = true;
                // the keyboard must NOT play in a buttons game
                int before = sw.screen.drops_received;
                app.key(A_SELECT); app.key(A_DOWN); step(100);
                CHECK(brain.state == State::HUMAN_TURN && sw.screen.drops_received == before, "keyboard ignored in a buttons game");
                snap("so_wrong_input_notice");
            }
            esp(BTN_CONFIRM);
        }
    }
    CHECK(brain.state == State::GAME_OVER, "screen-only game finished");
    printf("  reactions seen: %d, cut short: %d\n", watch.seen, watch.interrupted);
    CHECK(watch.interrupted == 0, "a reaction was cut short %d time(s)", watch.interrupted);
    snap("so_game_over");
    CHECK(gantry.drops_received == 0, "gantry got %d drops (must be 0)", gantry.drops_received);
    CHECK(gantry.last_selector == -1, "gantry never moved to a column");
    CHECK(gantry.homes_received == homesBefore, "gantry not homed during screen-only");
    CHECK(sw.screen.drops_received > 0, "on-screen simulator took the drops (%d)", sw.screen.drops_received);
    Stats st; st.load(sp);
    CHECK(st.roger[1].games() == 1, "screen-only game counted in stats");

    // 1v1 on screen only
    esp(BTN_CONFIRM); step(400);
    CHECK(app.screen == Screen::MENU && brain.state == State::MENU, "back at menu");
    esp(BTN_LEFT); esp(BTN_LEFT); esp(BTN_CONFIRM); esp(BTN_CONFIRM);   // Pro(2) -> 1v1(0)
    CHECK(app.screen == Screen::GAME && brain.mode == 0, "1v1 started");
    for (int k = 0; k < 3; k++) { esp(BTN_CONFIRM); step(500); esp(BTN_RIGHT); esp(BTN_CONFIRM); step(500); esp(BTN_LEFT); }
    snap("so_pvp");
    for (int i = 0; i < 400 && brain.state != State::GAME_OVER; i++) { if (brain.state == State::HUMAN_TURN) esp(BTN_CONFIRM); step(20); }
    snap("so_pvp_over");
    CHECK(brain.state == State::GAME_OVER && gantry.drops_received == 0, "1v1 finished, gantry untouched");

    // turning it off hands control back to the real gantry
    esp(BTN_CONFIRM); step(400);
    esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_CONFIRM);   // 1v1(0) -> Options(4)
    CHECK(app.screen == Screen::OPTIONS, "options again");
    esp(BTN_CONFIRM);                                         // chooser opens on the current choice (buttons)
    snap("screenmode_chooser_on");
    esp(BTN_RIGHT); esp(BTN_CONFIRM);                         // "Turn off screen only"
    CHECK(!app.screenOnly() && gantry.homes_received == homesBefore + 1, "gantry back on and re-homed");
    step(400);
    CHECK(brain.state == State::MENU, "gantry ready again");

    // recover from a gantry fault by switching to screen only
    gantry.fail_home = true; brain.rehome(); step(400);
    CHECK(brain.state == State::FAULT && app.screen == Screen::GAME, "gantry fault shown");
    esp(BTN_CONFIRM);
    CHECK(app.screen == Screen::OPTIONS, "fault -> select -> options");
    esp(BTN_CONFIRM); esp(BTN_CONFIRM); step(400);            // Screen only -> chooser -> Keyboard
    CHECK(app.screenOnly() && app.screenInput() == 0 && brain.state == State::MENU,
          "screen only (keyboard) recovers from the fault (%s)", stateName(brain.state));

    // keyboard game: the ESP32 buttons still run the menus, but not the game
    esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_CONFIRM);   // Options(4)... Back
    CHECK(app.screen == Screen::MENU, "back to menu with ESP32 buttons");
    app.key(A_UP); app.key(A_UP); app.key(A_UP); app.key(A_UP); step(20);  // Options(4) -> 1v1(0) by keyboard
    app.key(A_SELECT); app.key(A_SELECT); step(50);
    CHECK(app.screen == Screen::GAME && brain.mode == 0, "1v1 started from the keyboard");
    int before = sw.screen.drops_received;
    esp(BTN_CONFIRM); step(100);
    CHECK(sw.screen.drops_received == before && brain.state == State::HUMAN_TURN, "ESP32 button ignored in a keyboard game");
    app.key(A_SELECT); step(600);
    CHECK(sw.screen.drops_received == before + 1, "keyboard plays the keyboard game");
}


static void testReactionNotInterrupted(const std::string& facesDir) {
    printf("a reaction always plays to the end\n");
    SimLink sim; Settings set; Brain brain(sim, set, "/tmp/c4_ri_settings.conf", 3);
    FacePlayer faces; std::string m; faces.loadFolder(facesDir, m);
    Stats stats; App app(brain, faces, stats, "/tmp/c4_ri_stats.json", true);
    Canvas cv; uint64_t t = 1;
    auto run = [&](int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; app.frame(t, cv); } };
    run(100);
    app.testFace(FACE_BAD_MOVE);
    CHECK(faces.current == "bad_move", "reaction starts (%s)", faces.current.c_str());
    double total = 0; for (double d : faces.clips[faces.clipIndex].durations) total += d;
    run(100);
    app.testFace(FACE_THINKING);                 // R.O.G.E.R starts thinking mid-reaction
    app.testFace(FACE_IDLE);
    app.testFace(FACE_THINKING);
    run((int)total - 150);
    CHECK(faces.current == "bad_move", "still the reaction just before its last frame (%s)", faces.current.c_str());
    run(100);
    CHECK(faces.current == "thinking", "then the waiting face, thinking (%s)", faces.current.c_str());
    app.testFace(FACE_WIN);                      // thinking is not a reaction: switches right away
    CHECK(faces.current == "roger_win", "win face right away when no reaction is playing (%s)", faces.current.c_str());
    app.testFace(FACE_GOOD_MOVE); run(50); app.testFace(FACE_IDLE); run(50);
    CHECK(faces.current == "good_move", "idle waits too (%s)", faces.current.c_str());
    run(1500);
    CHECK(faces.current == "idle", "reaction ends on idle (%s)", faces.current.c_str());
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

    printf("screen flow with a simulated gantry\n");
    {
        SimLink sim; sim.home_ms = 100; sim.drop_ms = 100;
        Settings set; set.last_mode = 2;
        Brain brain(sim, set, "/tmp/c4_ui_settings.conf", 7);
        FacePlayer faces; std::string m; faces.loadFolder(facesDir, m);
        Stats stats; std::string sp = "/tmp/c4_ui_stats.json"; unlink(sp.c_str());
        App app(brain, faces, stats, sp, true);
        brain.buttonHook = [&app](uint8_t b) { return app.espButton(b); };
        Canvas cv;
        uint64_t t = 1;
        int shot = 0;
        ReactionWatch watch;
        auto step = [&](int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; brain.update(t); app.frame(t, cv); watch.tick(faces, t, app.screen == Screen::GAME); if (brain.searching()) usleep(500); } };
        auto snap = [&](const char* name) { if (shots.empty()) return; app.draw(cv, t); char p[256]; snprintf(p, sizeof p, "%s/%02d_%s.ppm", shots.c_str(), shot++, name); savePPM(cv, p); };
        auto esp = [&](uint8_t b) { sim.pushButton(b); step(30); };

        step(50);
        CHECK(app.screen == Screen::MENU, "starts on the menu");
        snap("menu_homing");
        step(1500);
        CHECK(brain.state == State::MENU, "gantry ready");
        snap("menu");
        // ESP32 buttons drive the menu: Pro is preselected; go down to Hacker and back up to Pro
        esp(BTN_RIGHT); esp(BTN_LEFT);
        app.key(A_DOWN); app.key(A_DOWN); app.key(A_DOWN); step(20);   // Options... Statistics
        app.key(A_SELECT); step(20);
        CHECK(app.screen == Screen::STATS, "stats screen");
        app.key(A_TEST); app.key(A_TEST); app.key(A_TEST); app.key(A_TEST); step(20);
        snap("stats");
        app.key(A_SELECT); step(20);
        CHECK(app.screen == Screen::MENU, "back to menu");
        app.key(A_UP); step(20); app.key(A_SELECT); step(20);
        CHECK(app.screen == Screen::OPTIONS, "options");
        snap("options");
        app.key(A_DOWN); app.key(A_SELECT); step(250);     // item 0 is "Screen only", item 1 is GIF test
        CHECK(app.screen == Screen::GIFTEST, "gif test");
        snap("giftest");
        app.key(A_DOWN); step(20); snap("giftest_next");
        app.key(A_SELECT); step(20);
        app.key(A_DOWN); app.key(A_SELECT); step(20);      // GIF test -> Shut down
        CHECK(app.screen == Screen::SHUTDOWN, "shutdown screen");
        snap("shutdown");
        app.key(A_SELECT); step(20);    // Cancel is preselected
        CHECK(app.screen == Screen::OPTIONS && !app.shutdownRequested, "cancel keeps running");
        app.key(A_DOWN); app.key(A_SELECT); step(20);  // from "Shut down" one down is Back
        CHECK(app.screen == Screen::MENU, "options back");

        // menu selection is still on Options (item 4); one LEFT on the ESP32 = Hacker
        esp(BTN_LEFT);
        CHECK(app.screen == Screen::MENU, "still on menu");
        snap("menu_hacker");
        esp(BTN_CONFIRM);
        CHECK(app.screen == Screen::CONFIRM, "confirm screen");
        snap("confirm");
        esp(BTN_CONFIRM);
        CHECK(app.screen == Screen::GAME, "game started");
        int mode = brain.mode;
        CHECK(mode == 3, "Hacker selected (mode %d)", mode);
        snap("game_start");

        // play: human always confirms the leftmost legal column
        bool sawThinking = false, sawHumanSelector = false; int games = 0;
        for (int i = 0; i < 4000 && brain.state != State::GAME_OVER; i++) {
            step(20);
            if (brain.state == State::AI_TURN && faces.current == "thinking") sawThinking = true;
            if (brain.state == State::HUMAN_TURN) {
                if (!sawHumanSelector) { snap("human_turn"); sawHumanSelector = true; }
                esp(BTN_CONFIRM);
            }
        }
        CHECK(brain.state == State::GAME_OVER, "game finished (%s)", stateName(brain.state));
        printf("  reactions seen: %d, cut short: %d\n", watch.seen, watch.interrupted);
        CHECK(watch.interrupted == 0, "a reaction was cut short %d time(s)", watch.interrupted);
        if (mode != 0) CHECK(sawThinking, "thinking face played during the search");
        step(200);
        snap("game_over");
        step(1200);                     // a reaction still playing at game over finishes first, then the win face
        if (mode != 0 && brain.winner == 0) CHECK(faces.current == "roger_win", "win face after any reaction (%s)", faces.current.c_str());
        Stats check; check.load(sp);
        games = check.totals().games() + check.pvpGames;
        CHECK(games == 5, "result recorded once (4 test games + 1 real, got %d)", games);
        esp(BTN_CONFIRM); step(50);
        CHECK(app.screen == Screen::MENU && brain.state == State::HOMING, "play again -> re-home -> menu");
        step(400);
        CHECK(brain.state == State::MENU, "ready again");

        // 1v1: the menu comes back on the last mode (Hacker = 3); three LEFTs = 1v1
        esp(BTN_LEFT); esp(BTN_LEFT); esp(BTN_LEFT); esp(BTN_CONFIRM); esp(BTN_CONFIRM);
        CHECK(app.screen == Screen::GAME && brain.mode == 0, "1v1 started (mode %d)", brain.mode);
        if (app.screen == Screen::GAME && brain.mode == 0) { step(100); snap("pvp"); }

        // fault screen
        sim.drop_error = true;
        for (int i = 0; i < 400 && brain.state != State::FAULT; i++) { step(20); if (brain.state == State::HUMAN_TURN) esp(BTN_CONFIRM); }
        CHECK(brain.state == State::FAULT && app.screen == Screen::GAME, "fault shown");
        snap("fault");
        esp(BTN_CONFIRM);
        CHECK(app.screen == Screen::OPTIONS, "fault -> select -> options");
        esp(BTN_RIGHT); esp(BTN_RIGHT); esp(BTN_CONFIRM);      // Screen only -> GIF test -> Shut down
        CHECK(app.screen == Screen::SHUTDOWN, "options -> shutdown screen");
        app.drawShuttingDown(cv); if (!shots.empty()) savePPM(cv, shots + "/99_shutting_down.ppm");
    }

    testReactionNotInterrupted(facesDir);
    testScreenOnly(facesDir, shots);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
