#include "Arduino.h"
#include "ESP32Servo.h"
FakeSerial Serial, Serial2; unsigned long g_ms = 0, g_us = 0; int g_pin[64]; long g_carriage = 3000; bool g_limit_broken = false;
std::vector<int> g_servo_writes;
#define main unused_main
#include "../src/main.cpp"
#undef main
#include <cassert>
static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { g_fail++; printf("FAIL: %s\n", msg); } else printf("ok: %s\n", msg); } while (0)

struct F { uint8_t cmd; std::vector<uint8_t> p; };
static std::vector<F> drain() {
    std::vector<F> out; FrameParser fp;
    for (uint8_t b : Serial2.tx) if (fp.feed_byte(b)) out.push_back({fp.cmd, std::vector<uint8_t>(fp.payload, fp.payload + fp.payload_len)});
    Serial2.tx.clear(); return out;
}
static void feed(uint8_t cmd, std::vector<uint8_t> p) { uint8_t f[16]; uint8_t n = build_frame(cmd, p.data(), p.size(), f); for (int i = 0; i < n; i++) Serial2.rx.push_back(f[i]); }
int main() {
    setup();
    auto o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "boot: homes then sends READY");
    // beacon repeats while Pi silent
    g_ms += 2100; loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "beacon repeats every 2 s");
    // Pi speaks: beacon stops forever
    feed(CMD_HOME, {}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "CMD_HOME -> READY");
    g_ms += 10000; loop(); o = drain();
    CHECK(o.empty(), "no beacon after hearing the Pi");
    // selector moves gantry, no reply
    feed(CMD_SELECTOR, {3}); loop(); o = drain();
    CHECK(o.empty() && g_carriage == columnSteps[3], "SELECTOR moves to column 3 silently");
    // drop: moves, servo, re-homes, DONE echoes col
    g_servo_writes.clear();
    feed(CMD_DROP, {5, 1}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && o[0].p[0] == 5, "DROP -> DONE(5)");
    CHECK(g_carriage <= 0, "re-homed after drop");
    CHECK(g_servo_writes.size() == 3 && g_servo_writes[0] == 120 && g_servo_writes[1] == 105 && g_servo_writes[2] == 90, "servo: BLACK select/push/rest");
    // invalid column
    feed(CMD_DROP, {9, 0}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].p[0] == ERR_INVALID_CMD, "bad column -> ERROR(INVALID)");
    // retired commands ignored
    feed(CMD_FACE, {3}); feed(CMD_MENU, {1}); feed(CMD_FAULT, {1}); feed(CMD_MODE_SELECTED, {2}); loop(); o = drain();
    CHECK(o.empty(), "retired commands ignored");
    // buttons forwarded
    g_pin[21] = LOW; g_ms += 100; loop(); g_pin[21] = HIGH; g_ms += 100; loop();
    g_pin[22] = LOW; g_ms += 100; loop(); g_pin[22] = HIGH; g_ms += 100; loop();
    o = drain();
    CHECK(o.size() == 2 && o[0].cmd == CMD_BUTTON && o[0].p[0] == 0 && o[1].p[0] == 2, "buttons -> CMD_BUTTON 0 and 2");
    // post-drop re-home failure => ERROR not DONE
    g_limit_broken = true; feed(CMD_DROP, {2, 0}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].p[0] == ERR_STEPPER_STALL, "re-home failure -> ERROR(STALL)");
    // not homed => NOT_HOMED
    feed(CMD_DROP, {2, 0}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].p[0] == ERR_NOT_HOMED, "drop while unhomed -> NOT_HOMED");
    // HOME while limit broken -> ERROR
    feed(CMD_HOME, {}); loop(); o = drain();
    CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR, "HOME failure -> ERROR");
    printf("%s\n", g_fail ? "FAILED" : "ALL OK"); return g_fail;
}
