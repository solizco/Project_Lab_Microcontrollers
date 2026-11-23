#pragma once
#include "Arduino.h"
extern std::vector<int> g_servo_writes;      // angles written, in order (-us for raw pulses)
struct Servo {
    bool on = false;
    void attach(int, int, int) { on = true; }
    void detach() { on = false; }
    bool attached() const { return on; }
    void write(int a) { g_servo_writes.push_back(a); }
    void writeMicroseconds(int us) { g_servo_writes.push_back(-us); }
};
