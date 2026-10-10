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

static uint64_t fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ULL; const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
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
        auto step = [&](int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; brain.update(t); app.frame(t, cv); if (brain.searching()) usleep(500); } };
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
        app.key(A_SELECT); step(250);
        CHECK(app.screen == Screen::GIFTEST, "gif test");
        snap("giftest");
        app.key(A_DOWN); step(20); snap("giftest_next");
        app.key(A_SELECT); step(20);
        app.key(A_DOWN); app.key(A_SELECT); step(20);
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
        if (mode != 0) CHECK(sawThinking, "thinking face played during the search");
        step(200);
        snap("game_over");
        if (mode != 0 && brain.winner == 0) CHECK(faces.current == "roger_win", "win face (%s)", faces.current.c_str());
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
        CHECK(app.screen == Screen::SHUTDOWN, "fault -> select -> shutdown screen");
        app.drawShuttingDown(cv); if (!shots.empty()) savePPM(cv, shots + "/99_shutting_down.ppm");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
