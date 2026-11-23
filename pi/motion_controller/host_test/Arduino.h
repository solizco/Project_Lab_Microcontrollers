#pragma once
// Minimal Arduino/ESP32 stand-ins so the firmware runs on a PC in simulated time.
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <string>
#include <vector>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define SERIAL_8N1 0
#define RTC_NOINIT_ATTR
#define constrain(x,a,b) ((x)<(a)?(a):((x)>(b)?(b):(x)))
struct FakeSerial {
    std::deque<uint8_t> rx; std::vector<uint8_t> tx; std::string out;   // out = USB log text
    void begin(...) {}
    int available() { return (int)rx.size(); }
    int read() { int v = rx.front(); rx.pop_front(); return v; }
    size_t write(const uint8_t* b, size_t n) { tx.insert(tx.end(), b, b + n); return n; }
    void flush() {}
    void print(const char* s) { out += s; } void print(int v) { out += std::to_string(v); }
    void println(const char* s) { out += s; out += "\n"; } void println(int v) { out += std::to_string(v) + "\n"; } void println() { out += "\n"; }
    int printf(const char* f, ...) { char b[600]; va_list a; va_start(a, f); int n = vsnprintf(b, sizeof b, f, a); va_end(a); out += b; return n; }
};
struct FakeESP { int restarts = 0; void restart() { restarts++; } };
extern FakeSerial Serial, Serial2; extern FakeESP ESP;
extern unsigned long g_ms, g_us;
inline unsigned long millis() { return g_ms; }
inline void delay(unsigned long ms) { g_ms += ms; }
inline void delayMicroseconds(unsigned int us) { g_us += us; g_ms += (g_us / 1000); g_us %= 1000; }
extern int g_pin[64]; extern long g_carriage, g_steps; extern bool g_limit_broken, g_driver_dead, g_en_rearms;
extern long g_press_at_step;          // a button press (GPIO21) happens while the motor turns
void onStep();
inline void pinMode(int p, int m) { if (m == INPUT_PULLUP) g_pin[p] = HIGH; }
inline void digitalWrite(int p, int v) {
    if (p == 25 && v == HIGH) { g_steps++; if (!g_driver_dead) g_carriage += (g_pin[26] ? 1 : -1); onStep(); }  // STEP with DIR
    if (p == 27 && v == LOW && g_pin[27] == HIGH && g_en_rearms) g_driver_dead = false;   // EN toggled
    g_pin[p] = v;
}
inline int digitalRead(int p) {
    if (p == 33) return (g_limit_broken || g_carriage > 0) ? HIGH : LOW;
    return g_pin[p];
}
