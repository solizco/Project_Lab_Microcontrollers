#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define SERIAL_8N1 0
#define constrain(x,a,b) ((x)<(a)?(a):((x)>(b)?(b):(x)))
struct FakeSerial {
    std::deque<uint8_t> rx; std::vector<uint8_t> tx;
    void begin(...) {} 
    int available() { return (int)rx.size(); }
    int read() { int v = rx.front(); rx.pop_front(); return v; }
    size_t write(const uint8_t* b, size_t n) { tx.insert(tx.end(), b, b + n); return n; }
    void print(const char*) {} void print(int) {} void println(const char*) {} void println(int) {} void println() {}
};
extern FakeSerial Serial, Serial2;
extern unsigned long g_ms, g_us;
inline unsigned long millis() { return g_ms; }
inline void delay(unsigned long ms) { g_ms += ms; }
inline void delayMicroseconds(unsigned int us) { g_us += us; g_ms += (g_us / 1000); g_us %= 1000; }
extern int g_pin[64]; extern long g_carriage; extern bool g_limit_broken;
inline void pinMode(int p, int m) { if (m == INPUT_PULLUP) g_pin[p] = HIGH; }
inline void digitalWrite(int p, int v) {
    if (p == 25 && v == HIGH) g_carriage += (g_pin[26] ? 1 : -1);   // STEP with DIR
    g_pin[p] = v;
}
inline int digitalRead(int p) {
    if (p == 33) return (g_limit_broken || g_carriage > 0) ? HIGH : LOW;
    return g_pin[p];
}
