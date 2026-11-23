#include "pins.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

const char* PINS_TEMPLATE =
"# ============================================================\n"
"# Connect 4 gantry pins for the Raspberry Pi 4\n"
"# ============================================================\n"
"# Fill in the BCM GPIO number for each wire (GPIO17 -> 17).\n"
"# These are NOT the physical pin numbers on the header.\n"
"# Leave a value blank if that part is not connected.\n"
"# Save the file, then restart the game (or the Pi). No rebuild needed.\n"
"#\n"
"# Rules:\n"
"#  - Buttons, the shutdown button and the limit switch connect the GPIO to GND\n"
"#    when pressed (the Pi turns on its internal pull-ups).\n"
"#  - SERVO on GPIO 12, 13, 18 or 19 uses the Pi's hardware PWM (smooth). Any\n"
"#    other pin works too, but the servo may twitch. After changing SERVO, run\n"
"#    install_autostart.sh again so it sets up the PWM for the new pin.\n"
"#  - Tip: put EN on GPIO 0-8. Those are pulled HIGH while the Pi boots, so the\n"
"#    motor stays off until the game starts.\n"
"#  - Never connect 5 V or the motor supply to a GPIO pin. 3.3 V logic only.\n"
"#  - Avoid GPIO 2 and 3 (I2C, and GPIO 3 is the optional Pi power button).\n"
"\n"
"# ---- stepper driver (TMC2209) ----\n"
"STEP =\n"
"DIR =\n"
"EN =\n"
"\n"
"# ---- limit switch (home) ----\n"
"LIMIT =\n"
"\n"
"# ---- dispenser servo (signal wire) ----\n"
"SERVO =\n"
"\n"
"# ---- buttons ----\n"
"BTN_LEFT =\n"
"BTN_RIGHT =\n"
"BTN_SELECT =\n"
"BTN_SHUTDOWN =\n"
"\n"
"# ---- mechanics (same as the ESP32 version; only change if the hardware changed) ----\n"
"STEPS_PER_MM = 39.46\n"
"FIRST_COLUMN_MM = 23.6\n"
"COLUMN_SPACING_MM = 33.3\n"
"STEP_DELAY_US = 500\n"
"HOMING_STEP_DELAY_US = 800\n"
"MAX_HOMING_STEPS = 11000\n"
"# 1 = the motor runs the wrong way (away from the limit switch when homing)\n"
"INVERT_DIR = 0\n"
"# 1 = TMC2209 style (EN low = motor on). 0 = EN high = motor on.\n"
"EN_ACTIVE_LOW = 1\n"
"# 1 = limit switch wired C + NC (reads pressed when it is not). Better: wire C + NO and keep 0.\n"
"LIMIT_INVERT = 0\n"
"# The switch must read pressed this many ms in a row (motor paused) to count.\n"
"# Rejects motor noise on the switch wire. Raise it if homing stops before the switch.\n"
"LIMIT_FILTER_MS = 3\n"
"SERVO_MIN_US = 500\n"
"SERVO_MAX_US = 2500\n"
"# 1 = keep the servo powered at rest between drops (holds the silos shut). 0 = go limp.\n"
"SERVO_HOLD = 1\n"
"# 1 = never use hardware PWM for the servo (only if hardware PWM misbehaves).\n"
"SERVO_FORCE_SOFTWARE = 0\n"
"# 1 = wiggle the servo a few degrees at start-up so you can see it is alive.\n"
"SERVO_BOOT_TEST = 0\n"
"# 1 = touch the limit switch, back off 3 mm, touch it again slowly. Noise can't fake both.\n"
"TWO_TOUCH_HOMING = 1\n"
"# A button must read pressed this many ms in a row to count (rejects motor noise).\n"
"BUTTON_STABLE_MS = 25\n"
"# 1 = pressing LEFT and RIGHT together is SELECT (BTN_SELECT can then stay blank).\n"
"CHORD_SELECT = 1\n"
"# The second arrow must land within this many ms of the first to count as select.\n"
"CHORD_WINDOW_MS = 150\n"
"# Motor off after this many seconds without moving (cooler driver, longer battery).\n"
"# The gantry homes again before its next move. 0 = never turn it off.\n"
"IDLE_MOTOR_OFF_S = 300\n";

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

std::vector<std::string> MotionConfig::missing() const {
    std::vector<std::string> m;
    if (step < 0) m.push_back("STEP");
    if (dir < 0) m.push_back("DIR");
    if (limit < 0) m.push_back("LIMIT");
    return m;
}

bool MotionConfig::load(const std::string& file) {
    std::ifstream f(file);
    if (!f) return false;
    path = file;
    std::map<std::string, int*> pins = {
        {"STEP", &step}, {"DIR", &dir}, {"EN", &en}, {"LIMIT", &limit}, {"SERVO", &servo},
        {"BTN_LEFT", &btnLeft}, {"BTN_RIGHT", &btnRight}, {"BTN_SELECT", &btnSelect}, {"BTN_SHUTDOWN", &btnShutdown}};
    std::map<std::string, float*> floats = {
        {"STEPS_PER_MM", &stepsPerMm}, {"FIRST_COLUMN_MM", &firstColumnMm}, {"COLUMN_SPACING_MM", &columnSpacingMm}};
    std::map<std::string, int*> ints = {
        {"STEP_DELAY_US", &stepDelayUs}, {"HOMING_STEP_DELAY_US", &homingStepDelayUs},
        {"MAX_HOMING_STEPS", &maxHomingSteps}, {"SERVO_MIN_US", &servoMinUs}, {"SERVO_MAX_US", &servoMaxUs}};
    struct Ranged { int* v; int max; };
    std::map<std::string, Ranged> ints0 = {{"LIMIT_FILTER_MS", {&limitFilterMs, 100}},   // 0 allowed
                                           {"BUTTON_STABLE_MS", {&buttonStableMs, 500}},
                                           {"CHORD_WINDOW_MS", {&chordWindowMs, 1000}},
                                           {"IDLE_MOTOR_OFF_S", {&idleMotorOffS, 86400}}};
    std::map<std::string, bool*> bools = {
        {"INVERT_DIR", &invertDir}, {"EN_ACTIVE_LOW", &enActiveLow}, {"LIMIT_INVERT", &limitInvert},
        {"SERVO_HOLD", &servoHold}, {"SERVO_FORCE_SOFTWARE", &servoForceSoftware}, {"SERVO_BOOT_TEST", &servoBootTest},
        {"TWO_TOUCH_HOMING", &twoTouchHoming}, {"CHORD_SELECT", &chordSelect}};
    std::string line; int n = 0;
    std::vector<std::string> order;        // pins in the order the file sets them
    while (std::getline(f, line)) {
        n++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { problems.push_back("line " + std::to_string(n) + ": no '=' in \"" + line + "\""); continue; }
        std::string key = upper(trim(line.substr(0, eq))), val = trim(line.substr(eq + 1));
        if (!val.empty() && (val[0] == 'G' || val[0] == 'g')) {           // allow "GPIO17"
            std::string u = upper(val);
            if (u.rfind("GPIO", 0) == 0) val = trim(val.substr(4));
        }
        auto bad = [&](const std::string& why) { problems.push_back(key + " = " + val + ": " + why); };
        if (pins.count(key)) {
            if (val.empty()) { *pins[key] = -1; continue; }
            char* end = nullptr; long v = strtol(val.c_str(), &end, 10);
            if (*end || v < 0 || v > 27) { bad("must be a GPIO number from 0 to 27 (or blank)"); *pins[key] = -1; continue; }
            *pins[key] = (int)v;
            order.push_back(key);
        } else if (floats.count(key)) {
            if (val.empty()) continue;
            char* end = nullptr; double v = strtod(val.c_str(), &end);
            if (*end || v <= 0) { bad("must be a positive number"); continue; }
            *floats[key] = (float)v;
        } else if (ints.count(key)) {
            if (val.empty()) continue;
            char* end = nullptr; long v = strtol(val.c_str(), &end, 10);
            if (*end || v <= 0) { bad("must be a positive whole number"); continue; }
            *ints[key] = (int)v;
        } else if (ints0.count(key)) {
            if (val.empty()) continue;
            char* end = nullptr; long v = strtol(val.c_str(), &end, 10);
            if (*end || v < 0 || v > ints0[key].max) { bad("must be a whole number from 0 to " + std::to_string(ints0[key].max)); continue; }
            *ints0[key].v = (int)v;
        } else if (bools.count(key)) {
            if (val.empty()) continue;
            if (val != "0" && val != "1") { bad("must be 0 or 1"); continue; }
            *bools[key] = (val == "1");
        } else {
            problems.push_back("unknown setting \"" + key + "\" (line " + std::to_string(n) + ")");
        }
    }
    // the same GPIO used twice is always a wiring mistake
    std::map<int, std::string> used;
    for (auto& k : order) {                // the later line loses
        int* p = pins[k];
        if (*p < 0) continue;
        if (used.count(*p) && used[*p] != k) {
            problems.push_back("GPIO" + std::to_string(*p) + " is used for both " + used[*p] + " and " + k + ": " + k + " ignored");
            *p = -1;
        } else used[*p] = k;
    }
    if (chordSelect && chordWindowMs < 30) { problems.push_back("CHORD_WINDOW_MS raised to 30"); chordWindowMs = 30; }
    if (idleMotorOffS > 0 && idleMotorOffS < 30) { problems.push_back("IDLE_MOTOR_OFF_S raised to 30"); idleMotorOffS = 30; }
    if (stepDelayUs < 50) { problems.push_back("STEP_DELAY_US raised to 50"); stepDelayUs = 50; }
    if (homingStepDelayUs < 50) { problems.push_back("HOMING_STEP_DELAY_US raised to 50"); homingStepDelayUs = 50; }
    if (servoMinUs < 300 || servoMaxUs > 2700) problems.push_back("SERVO_MIN_US / SERVO_MAX_US look unusual (most servos: 500-2500)");
    if (servoMaxUs <= servoMinUs) { problems.push_back("SERVO_MAX_US must be above SERVO_MIN_US: using 500/2500"); servoMinUs = 500; servoMaxUs = 2500; }
    return true;
}

static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

std::string MotionConfig::find(const std::string& exeDir) {
    std::vector<std::string> c = {"/boot/firmware/roger/pins.conf", "/boot/roger/pins.conf"};
    const char* home = getenv("HOME");
    if (home) c.push_back(std::string(home) + "/roger/pins.conf");
    if (!exeDir.empty()) { c.push_back(exeDir + "/../pins.conf"); c.push_back(exeDir + "/pins.conf"); }
    for (auto& p : c) if (exists(p)) return p;
    return "";
}

std::string MotionConfig::summary() const {
    auto g = [](int p) { return p < 0 ? std::string("-") : std::to_string(p); };
    char b[400];
    snprintf(b, sizeof b, "STEP %s  DIR %s  EN %s  LIMIT %s  SERVO %s  LEFT %s  RIGHT %s  SELECT %s  SHUTDOWN %s",
             g(step).c_str(), g(dir).c_str(), g(en).c_str(), g(limit).c_str(), g(servo).c_str(),
             g(btnLeft).c_str(), g(btnRight).c_str(), g(btnSelect).c_str(), g(btnShutdown).c_str());
    return b;
}

// ---------------- servo calibration file ----------------
std::string servoCalPath() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/connect4/servo.conf";
}

bool loadServoCal(const std::string& path, ServoCal& cal) {
    std::ifstream f(path);
    if (!f) return false;
    ServoCal t = cal; std::string line; int got = 0;
    std::map<std::string, int*> k = {{"rest", &t.rest}, {"red_select", &t.redSel}, {"red_push", &t.redPush},
                                     {"black_select", &t.blackSel}, {"black_push", &t.blackPush}, {"wait_ms", &t.waitMs}};
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (!k.count(key)) continue;
        char* end = nullptr; long v = strtol(val.c_str(), &end, 10);
        if (*end || val.empty()) continue;
        bool isWait = key == "wait_ms";
        if (isWait ? (v < 50 || v > 3000) : (v < 0 || v > 180)) continue;
        *k[key] = (int)v; got++;
    }
    if (!got) return false;
    cal = t;
    return true;
}

static void mkdirsFor(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool saveServoCal(const std::string& path, const ServoCal& c) {
    mkdirsFor(path);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    fprintf(f, "# Servo positions (degrees) saved from Options > Servo calibration\n");
    fprintf(f, "rest=%d\nred_select=%d\nred_push=%d\nblack_select=%d\nblack_push=%d\nwait_ms=%d\n",
            c.rest, c.redSel, c.redPush, c.blackSel, c.blackPush, c.waitMs);
    bool ok = fflush(f) == 0;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}
