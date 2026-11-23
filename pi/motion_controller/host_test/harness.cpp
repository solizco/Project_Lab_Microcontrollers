// PC test of the Motion Controller firmware: g++ -std=c++17 -I host_test host_test/harness.cpp
#include "Arduino.h"
#include "ESP32Servo.h"
#include "Preferences.h"
#include <cctype>
FakeSerial Serial, Serial2; FakeESP ESP; unsigned long g_ms = 0, g_us = 0; int g_pin[64];
long g_carriage = 3000, g_steps = 0, g_press_at_step = -1; bool g_limit_broken = false, g_driver_dead = false, g_en_rearms = true;
int g_reset_reason = 1; std::vector<int> g_servo_writes; std::map<std::string, int> g_nvs;
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
    bootHomePending = false; autoHomeTries = 0; ledMode = LED_BUSY; g_pin[2] = LOW;
}

int main() {
    // ---- normal boot and protocol ----
    setup();
    auto o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "boot: homes then sends READY");
    CHECK(!dbg.empty() && dbg[0].p[0] == DBG_BOOT && dbg[0].p[2] == 3, "boot: DBG_BOOT first, firmware build 3");
    loop();
    CHECK(g_pin[2] == HIGH && ledMode == LED_OK, "status LED solid once homed");
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
    // it keeps retrying by itself; once the driver wakes up, it homes and tells the Pi
    feed(CMD_PING, {}); loop(); drain();                       // the Pi has spoken: beacon is off
    for (int i = 0; i < 5; i++) { g_ms += 3100; loop(); }
    o = drain();
    CHECK(!homed && o.empty() && autoHomeTries >= 4 && ledMode == LED_FAIL, "still failing: retries every 3 s, LED slow blink, nothing sent");
    g_driver_dead = false;
    g_ms += 3100; loop(); o = drain();
    CHECK(homed && o.size() == 1 && o[0].cmd == CMD_READY && ledMode == LED_OK, "driver wakes up: next automatic retry homes and sends READY");
    g_ms += 10000; loop(); o = drain();
    CHECK(o.empty(), "no more retries once homed");
    resetBoard(); g_driver_dead = false; g_reset_reason = 1; setup(); o = drain();
    CHECK(bootRestarts == 0 && homed && o[0].cmd == CMD_READY, "a real power-up starts counting again");

    // ---- servo calibration from the serial monitor ----
    auto type = [](const char* line) { for (const char* p = line; *p; p++) Serial.rx.push_back(*p); Serial.rx.push_back('\n'); loop(); };
    g_servo_writes.clear(); Serial.out.clear();
    type("a100");
    CHECK(!g_servo_writes.empty() && g_servo_writes.back() == 100 && servoNow == 100, "a100 moves and holds the servo at 100");
    type("++"); type("-");
    CHECK(servoNow == 104 && g_servo_writes.back() == 104, "++ then - nudges to 104");
    type("rest");
    CHECK(cal.rest == 104, "rest stores the current angle");
    type("a40"); type("red"); type("a55"); type("redpush"); type("a140"); type("black"); type("a125"); type("blackpush"); type("wait400");
    CHECK(cal.redSel == 40 && cal.redPush == 55 && cal.blackSel == 140 && cal.blackPush == 125 && cal.waitMs == 400, "all positions and the wait stored");
    g_servo_writes.clear(); unsigned long t1 = g_ms; type("tr");
    CHECK(g_servo_writes.size() == 3 && g_servo_writes[0] == 40 && g_servo_writes[1] == 55 && g_servo_writes[2] == 104, "tr: red select, red push, back to rest");
    CHECK(g_ms - t1 >= 1200, "waits 400 ms after each move");
    g_servo_writes.clear(); feed(CMD_DROP, {2, 1}); loop(); drain();
    CHECK(g_servo_writes.size() == 3 && g_servo_writes[0] == 140 && g_servo_writes[1] == 125 && g_servo_writes[2] == 104, "a real game drop uses the calibrated black angles");
    type("u1200");
    CHECK(g_servo_writes.back() == -1200 && servoNow == -1, "u1200 sends a raw 1200 us pulse");
    Serial.out.clear(); type("rest");
    CHECK(cal.rest == 104 && Serial.out.find("Move the servo with a<angle> first") != std::string::npos, "after a raw pulse, store asks for an angle first");
    CHECK(g_nvs.empty(), "nothing written to flash before save");
    Serial.out.clear(); type("save");
    CHECK(g_nvs["rest"] == 104 && g_nvs["blackPush"] == 125 && g_nvs["waitMs"] == 400, "save writes to flash");
    CHECK(Serial.out.find("{ 104, 40, 55, 140, 125, 400 }") != std::string::npos, "save prints the line to paste into main.cpp");
    cal = SERVO_DEFAULTS; resetBoard(); g_reset_reason = 1; g_servo_writes.clear(); setup(); drain();
    CHECK(cal.rest == 104 && cal.blackSel == 140 && g_servo_writes.front() == 104, "after a power cycle the saved values are loaded and the servo starts at rest 104");
    type("defaults");
    CHECK(cal.rest == 90 && cal.redSel == 60 && servoNow == 90, "defaults restores the built-in values");
    type("off");
    CHECK(!dispenserServo.attached() && servoNow == -1, "off stops the pulses");
    g_servo_writes.clear(); type("tb");
    CHECK(dispenserServo.attached() && g_servo_writes.size() == 3, "a drop turns the servo back on");
    Serial.out.clear(); type("r5");
    CHECK(ESP.restarts == 0 && Serial.out.find("Unknown command") != std::string::npos, "a typo starting with r does not restart the board");
    g_nvs.clear(); cal = SERVO_DEFAULTS;

    // ---- USB console ----
    g_carriage = 500;
    for (char c : std::string("h\nm4\n")) Serial.rx.push_back(c); loop(); drain();
    CHECK(g_carriage == columnSteps[4], "console m4 moves to column 4");
    printf("%s\n", g_fail ? "FAILED" : "ALL OK"); return g_fail;
}
