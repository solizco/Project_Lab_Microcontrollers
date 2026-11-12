// PC test of the Motion Controller firmware: g++ -std=c++17 -I host_test host_test/harness.cpp
#include "Arduino.h"
#include "ESP32Servo.h"
FakeSerial Serial, Serial2; FakeESP ESP; unsigned long g_ms = 0, g_us = 0; int g_pin[64];
long g_carriage = 3000, g_steps = 0, g_press_at_step = -1; bool g_limit_broken = false, g_driver_dead = false, g_en_rearms = true;
int g_reset_reason = 1; std::vector<int> g_servo_writes;
void onStep() {
    if (g_press_at_step >= 0 && g_steps == g_press_at_step) g_pin[21] = LOW;          // press LEFT mid-move
    if (g_press_at_step >= 0 && g_steps == g_press_at_step + 400) { g_pin[21] = HIGH; g_press_at_step = -1; }
}
#define main unused_main
#include "../src/main.cpp"
#undef main
static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { g_fail++; printf("FAIL: %s\n", msg); } else printf("ok: %s\n", msg); } while (0)

struct F { uint8_t cmd; std::vector<uint8_t> p; };
static std::vector<F> dbg;
static std::vector<F> drain() {           // frames to the Pi, debug frames split off into dbg
    std::vector<F> out; FrameParser fp; dbg.clear();
    for (uint8_t b : Serial2.tx) if (fp.feed_byte(b)) {
        F f{fp.cmd, std::vector<uint8_t>(fp.payload, fp.payload + fp.payload_len)};
        (f.cmd == CMD_DEBUG ? dbg : out).push_back(f);
    }
    Serial2.tx.clear(); return out;
}
static void feed(uint8_t cmd, std::vector<uint8_t> p) { uint8_t f[16]; uint8_t n = build_frame(cmd, p.data(), p.size(), f); for (int i = 0; i < n; i++) Serial2.rx.push_back(f[i]); }
static void resetBoard() {   // power the simulated board back up
    Serial2.tx.clear(); Serial2.rx.clear(); Serial.out.clear(); ESP.restarts = 0;
    heardPi = false; homed = false; currentPosition = 0; parser = FrameParser();
}

int main() {
    // ---- normal boot and protocol ----
    setup();
    auto o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "boot: homes then sends READY");
    CHECK(!dbg.empty() && dbg[0].p[0] == DBG_BOOT, "boot: DBG_BOOT first");
    g_ms += 2100; loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "beacon repeats every 2 s");
    feed(CMD_HOME, {}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "CMD_HOME -> READY");
    g_ms += 10000; loop(); o = drain();
    CHECK(o.empty(), "no beacon after hearing the Pi");
    feed(CMD_PING, {}); loop(); o = drain();
    CHECK(o.empty() && dbg.size() == 2 && dbg[1].p[0] == DBG_STATUS && (dbg[1].p[1] & 1), "PING -> DBG_STATUS homed");
    feed(CMD_SELECTOR, {3}); loop(); o = drain();
    CHECK(o.empty() && g_carriage == columnSteps[3], "SELECTOR moves to column 3 silently");
    CHECK(columnSteps[0] == 931 && columnSteps[6] == 8815, "23.6 mm first-column offset kept");
    g_servo_writes.clear();
    feed(CMD_DROP, {5, 1}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && o[0].p[0] == 5, "DROP -> DONE(5)");
    CHECK(g_carriage <= 0, "re-homed after drop");
    CHECK(g_servo_writes.size() == 3 && g_servo_writes[0] == 120 && g_servo_writes[2] == 90, "servo: BLACK select/push/rest");

    // ---- bug fix: buttons keep working while the gantry is moving ----
    g_press_at_step = g_steps + 2000;                       // LEFT pressed 2000 steps into the move
    feed(CMD_DROP, {6, 0}); loop(); o = drain();
    bool buttonBeforeDone = o.size() == 2 && o[0].cmd == CMD_BUTTON && o[0].p[0] == 0 && o[1].cmd == CMD_DONE;
    CHECK(buttonBeforeDone, "a press DURING a move reaches the Pi right away (before DONE)");
    g_pin[21] = HIGH;
    // shutdown button held during a long move is sent too
    g_pin[19] = LOW; feed(CMD_DROP, {6, 1}); loop(); g_pin[19] = HIGH; loop(); o = drain();
    bool sd = false; for (auto& f : o) sd |= f.cmd == CMD_SHUTDOWN;
    CHECK(sd, "shutdown button held during a move is still sent");

    // ---- failures ----
    g_limit_broken = true; feed(CMD_DROP, {2, 0}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].p[0] == ERR_STEPPER_STALL, "re-home failure -> ERROR(STALL)");
    unsigned long t0 = g_ms; feed(CMD_HOME, {}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR, "HOME failure -> ERROR");
    CHECK(g_ms - t0 < 32000, "3 failed homing attempts take ~28 s, not ~50 s");
    g_limit_broken = false;

    // ---- bug fix: cold start where the driver ignores steps until EN is toggled ----
    resetBoard(); g_carriage = 5000; g_driver_dead = true; g_en_rearms = true; g_reset_reason = 1;
    setup(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY && homed, "driver re-armed by armDriver(): boot homing OK");

    // ---- bug fix: cold start that only a restart fixes (EN press) ----
    resetBoard(); g_carriage = 5000; g_driver_dead = true; g_en_rearms = false; g_reset_reason = 1;
    setup();
    CHECK(ESP.restarts == 1 && bootRestarts == 1, "boot homing failed: the board restarts itself (like pressing EN)");
    resetBoard(); g_reset_reason = 3; setup();                // the software restart, still broken
    CHECK(ESP.restarts == 1 && bootRestarts == 2, "...a second time");
    resetBoard(); g_reset_reason = 3; setup(); o = drain();
    CHECK(ESP.restarts == 0 && !o.empty() && o[0].cmd == CMD_ERROR, "then stops restarting and reports the failure");
    resetBoard(); g_driver_dead = false; g_reset_reason = 1; setup(); o = drain();
    CHECK(bootRestarts == 0 && homed && o[0].cmd == CMD_READY, "a real power-up starts counting again");

    // ---- USB console ----
    g_carriage = 500;
    for (char c : std::string("h\nm4\n")) Serial.rx.push_back(c); loop(); drain();
    CHECK(g_carriage == columnSteps[4], "console m4 moves to column 4");
    printf("%s\n", g_fail ? "FAILED" : "ALL OK"); return g_fail;
}
