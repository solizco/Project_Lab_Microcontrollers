#pragma once
#include "Arduino.h"
extern std::vector<int> g_servo_writes;
struct Servo { void attach(int,int,int){} void write(int a){ g_servo_writes.push_back(a);} };
