#pragma once
// ============================================================
// pins.h -- gantry wiring and mechanics, read from pins.conf
// ============================================================
// The Pi 4 drives the gantry directly (no ESP32). Which GPIO does what is
// NOT compiled in: it comes from a plain text file you edit, so changing a
// wire never needs a rebuild. The file is looked for in this order:
//   /boot/firmware/roger/pins.conf   (on the SD card: edit it from Windows)
//   /boot/roger/pins.conf
//   ~/roger/pins.conf
//   pins.conf next to the brain program
// Pin numbers are BCM GPIO numbers (GPIO17 -> 17), NOT physical pin numbers.
// A blank value means "not connected".
#include <string>
#include <vector>

struct ServoCal { int rest = 103, redSel = 70, redPush = 100, blackSel = 135, blackPush = 110, waitMs = 250; };

struct MotionConfig {
    // ---- pins (BCM GPIO numbers, -1 = blank / not connected) ----
    int step = -1, dir = -1, en = -1, limit = -1, servo = -1;
    int btnLeft = -1, btnRight = -1, btnSelect = -1, btnShutdown = -1;
    // ---- mechanics (same numbers as the ESP32 firmware) ----
    float stepsPerMm = 39.46f;
    float firstColumnMm = 23.6f;      // home (limit switch) to column 0
    float columnSpacingMm = 33.3f;
    int stepDelayUs = 500;            // normal moves
    int homingStepDelayUs = 800;
    int maxHomingSteps = 11000;
    bool invertDir = false;           // motor runs the wrong way: set INVERT_DIR = 1
    bool enActiveLow = true;          // TMC2209 EN: low = motor on
    bool limitInvert = false;         // switch wired normally-closed: reads backwards
    int limitFilterMs = 3;            // switch must read pressed this long, motor paused (rejects motor noise)
    int servoMinUs = 500, servoMaxUs = 2500;   // pulse width for 0 and 180 degrees
    bool servoHold = true;            // keep pulsing at rest between drops (0: go limp, less buzz and current)
    bool servoForceSoftware = false;  // skip hardware PWM (only if it misbehaves on this Pi)
    bool servoBootTest = false;       // small wiggle at start-up, so you can see the servo is alive
    bool twoTouchHoming = true;       // touch the switch, back off, touch again slowly (noise can't fake both)
    int buttonStableMs = 25;          // a button must read pressed this long to count (rejects motor noise)
    bool chordSelect = true;          // left + right pressed together = select
    int chordWindowMs = 150;          // how close together the two presses must land
    int idleMotorOffS = 300;          // motor off after this long without moving (0 = never); homes before the next move

    std::string path;                 // file that was read ("" = none found)
    std::vector<std::string> problems; // human-readable issues (bad values, duplicates)

    // Enough to home and move: STEP, DIR and the limit switch.
    bool motionReady() const { return step >= 0 && dir >= 0 && limit >= 0; }
    std::vector<std::string> missing() const;   // names of required pins left blank

    // Parse a file. Unknown keys and bad values are reported in problems.
    bool load(const std::string& file);
    // First existing file from the search list above (exeDir = folder of the program).
    static std::string find(const std::string& exeDir);
    std::string summary() const;      // one line per pin, for the log and Link debug
};

// Blank template written next to the program when no pins.conf exists anywhere.
extern const char* PINS_TEMPLATE;

// Servo calibration file: ~/.config/connect4/servo.conf (saved from the menu).
std::string servoCalPath();
bool loadServoCal(const std::string& path, ServoCal& cal);
bool saveServoCal(const std::string& path, const ServoCal& cal);
