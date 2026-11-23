// ============================================================
// Motion Controller (ESP32-WROOM-32E): MOTION ONLY
// ============================================================
// The Raspberry Pi owns the game, the menu and the display. This board:
//   - homes the gantry (limit switch) and reports CMD_READY
//   - moves to a column for CMD_SELECTOR (live tracking of the human's choice)
//   - executes CMD_DROP: move, servo drop, RE-HOME, then CMD_DONE (or CMD_ERROR)
//   - forwards the three buttons to the Pi as CMD_BUTTON, in EVERY state
//   - shutdown button (GPIO19, hold 2 s): tells the Pi to power off cleanly
//   - keeps forwarding the buttons (and the shutdown button) WHILE the gantry
//     moves or homes, so the Pi's menus never go dead during a long move
//   - a cold start that can't home is retried, then the board restarts itself
//     (what pressing EN used to fix), at most twice per power-up
//   - debugging (firmware v2): reports what it is doing to the Pi as CMD_DEBUG
//     frames, answers CMD_PING, restarts on CMD_RESET, and logs everything on
//     the USB serial monitor (115200) with a status line every 5 s. Type ? in
//     the serial monitor for test commands that work without the Pi.
//   - status LED (GPIO2, the blue LED on most ESP32 boards): on from the very
//     first line of setup(). Fast blink = waiting/homing, solid = homed,
//     slow blink = homing failed (it keeps retrying by itself). If the LED is
//     DARK after power-up, this program is not running at all: the ESP32
//     itself did not start, which is a power/reset problem, not firmware.
//   - servo calibration from the serial monitor (type "cal" for the commands):
//     move the servo by angle or by pulse width, store rest / red / black
//     positions, test a drop, and "save" them to flash. No re-upload needed.
// It knows nothing about Connect Four rules or what is on screen.
//
// Retired commands (CMD_FACE / CMD_MENU / CMD_FAULT / CMD_MODE_SELECTED)
// are ignored if they ever arrive.
#include <Arduino.h>
#include <ESP32Servo.h>
#include <Preferences.h>   // servo calibration is saved in flash (NVS)
#include <esp_system.h>
#include <esp_attr.h>     // RTC_NOINIT_ATTR
#include "protocol.h"

// ---------- Pins (confirmed working on the validated rigs) ----------
#define STEP_PIN         25
#define DIR_PIN          26
#define EN_PIN           27   // TMC2209 EN is active low
#define LIMIT_SWITCH_PIN 33
#define SERVO_PIN        32

#define BTN_LEFT_PIN   21
#define BTN_RIGHT_PIN  23
#define BTN_SELECT_PIN 22
// Status LED: GPIO2 is the onboard blue LED on most ESP32 dev boards.
#define LED_PIN 2
// Firmware build, shown on the serial monitor and on the Pi's Link debug screen.
#define FW_BUILD 3

// Shutdown button: hold 2 s and the Pi powers off cleanly (for swapping SD cards).
// Wired from GPIO19 to GND, internal pull-up. GPIO19 was the old display's MOSI.
#define BTN_SHUTDOWN_PIN 19
#define SHUTDOWN_HOLD_MS 2000

// UART to the Pi: Pi GPIO14 (TX) -> GPIO14 here (RX); Pi GPIO15 (RX) <- GPIO5 here (TX).
// GPIO2 fights the bootloader and GPIO34/35/36/39 are input-only, so TX is GPIO5.
#define PI_UART_BAUD 115200
#define PI_UART_RX   14
#define PI_UART_TX   5

// ---------- Motion constants (measured) ----------
const float STEPS_PER_MM = 39.46;
const float FIRST_COLUMN_OFFSET_MM = 23.6;   // home (limit switch) to the FIRST column (column 0)
const float COLUMN_SPACING_MM = 33.3;        // from one column to the next
const int NUM_COLUMNS = 7;
long columnSteps[NUM_COLUMNS];

const unsigned int STEP_DELAY_US = 500;
const unsigned int HOMING_STEP_DELAY_US = 800;
// Full travel to column 6 is ~8800 steps; give up a little past that (~9 s at
// homing speed) instead of 20000 steps (16 s), so a failed home is noticed and
// retried quickly.
#define MAX_HOMING_STEPS 11000

// ---------- Dispenser servo calibration ----------
// A drop is: go to the color's SELECT angle, then its PUSH angle, then back to
// REST, waiting `servoWaitMs` after each move. These are the defaults; the
// values actually used come from flash once you "save" a calibration.
// color 0 = RED, color 1 = BLACK.
#define SERVO_MIN_US 500      // pulse width for 0 deg
#define SERVO_MAX_US 2500     // pulse width for 180 deg
struct ServoCal { int rest, redSel, redPush, blackSel, blackPush, waitMs; };
const ServoCal SERVO_DEFAULTS = { 90, 60, 75, 120, 105, 250 };
ServoCal cal = SERVO_DEFAULTS;
int servoNow = -1;            // last angle commanded (-1: off / raw pulse)
Preferences prefs;

Servo dispenserServo;
FrameParser parser;
long currentPosition = 0;
bool homed = false;

// ---------- Debug counters ----------
unsigned long rxBytes = 0, rxFrames = 0, txFrames = 0;
uint32_t lastBadReported = 0;
unsigned long lastBadSent = 0, lastStatusPrint = 0;
#define STATUS_PRINT_MS 5000
uint8_t bootReason = 0;
const char* cmdName(uint8_t c) {
    switch (c) {
        case CMD_HOME: return "HOME"; case CMD_DROP: return "DROP"; case CMD_SELECTOR: return "SELECTOR";
        case CMD_BUTTON: return "BUTTON"; case CMD_READY: return "READY"; case CMD_DONE: return "DONE";
        case CMD_ERROR: return "ERROR"; case CMD_SHUTDOWN: return "SHUTDOWN"; case CMD_PING: return "PING";
        case CMD_RESET: return "RESET"; case CMD_DEBUG: return "DEBUG";
        case CMD_FACE: case CMD_MENU: case CMD_FAULT: case CMD_MODE_SELECTED: return "RETIRED";
        default: return "UNKNOWN";
    }
}
const char* resetName(uint8_t r) {
    switch (r) {
        case 1: return "power-on"; case 2: return "external pin"; case 3: return "software restart";
        case 4: return "crash"; case 5: case 6: case 7: return "watchdog"; case 8: return "deep sleep";
        case 9: return "BROWNOUT (supply voltage dipped)"; default: return "unknown";
    }
}

// ---------- READY beacon ----------
// Boot order is not guaranteed (the Pi takes 20-30 s to boot). So after
// boot homing we keep announcing the result every 2 s until the Pi has said
// ANYTHING to us. As soon as one valid frame arrives the beacon stops for
// good, so it can never interleave with a real reply mid-game.
bool heardPi = false;
bool bootHomeOk = false;
unsigned long lastBeacon = 0;
#define BEACON_INTERVAL_MS 2000

// ---------- Buttons ----------
int lastLeft, lastRight, lastSelect;
const unsigned long debounceMs = 40;

bool debouncedPress(int pin, int &lastState) {
    int s = digitalRead(pin);
    unsigned long now = millis();
    static unsigned long lastChange[3] = {0, 0, 0};
    int idx = (pin == BTN_LEFT_PIN) ? 0 : (pin == BTN_RIGHT_PIN) ? 1 : 2;
    if (s != lastState && now - lastChange[idx] > debounceMs) {
        lastChange[idx] = now;
        lastState = s;
        return s == LOW;
    }
    return false;
}

// ---------- Protocol: outgoing ----------
void send_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t flen = build_frame(cmd, payload, len, frame);
    Serial2.write(frame, flen);
    txFrames++;
    if (cmd != CMD_DEBUG) {                   // debug frames would double every line
        Serial.printf("[%7lu] TX -> Pi: %s", millis(), cmdName(cmd));
        for (int i = 0; i < len; i++) Serial.printf(" %u", payload[i]);
        Serial.println();
    }
}
void send_debug(uint8_t ev, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) {
    uint8_t p[4] = { ev, a, b, c };
    send_frame(CMD_DEBUG, p, 4);
}
bool limitPressed() { return digitalRead(LIMIT_SWITCH_PIN) == LOW; }

// Filtered read for homing: the switch must read LOW 3 times in a row
// (100 us apart) to count as pressed, so a noise spike can't fake a home.
bool limitPressedFiltered() {
    for (int i = 0; i < 3; i++) {
        if (digitalRead(LIMIT_SWITCH_PIN) != LOW) return false;
        delayMicroseconds(100);
    }
    return true;
}
void send_status() {
    uint8_t bad = parser.bad_frames > 255 ? 255 : (uint8_t)parser.bad_frames;
    send_debug(DBG_STATUS, (homed ? 1 : 0) | (limitPressed() ? 2 : 0), FW_BUILD, bad);
}
void send_ready()           { send_frame(CMD_READY, nullptr, 0); }
void send_done(uint8_t col) { uint8_t p[1] = { col };  send_frame(CMD_DONE, p, 1); }
void send_error(uint8_t c)  { uint8_t p[1] = { c };    send_frame(CMD_ERROR, p, 1); }
void send_shutdown()        { send_frame(CMD_SHUTDOWN, nullptr, 0); }
void send_button(uint8_t b) { uint8_t p[1] = { b };    send_frame(CMD_BUTTON, p, 1); }

// ---------- Buttons are read everywhere, even mid-move ----------
// Moves and homing block for seconds. Before, the buttons were only read in
// loop(), so during a long (or failing) home every press was lost and the
// Pi's menus looked frozen. Now every step loop calls this every ~2 ms.
unsigned long shutdownDownSince = 0, lastShutdownSent = 0, lastService = 0;
enum LedMode { LED_BUSY, LED_OK, LED_FAIL };   // fast blink / solid / slow blink
LedMode ledMode = LED_BUSY;
void updateLed() {
    unsigned long t = millis();
    digitalWrite(LED_PIN, ledMode == LED_OK ? HIGH : ledMode == LED_BUSY ? ((t / 100) % 2 ? HIGH : LOW)
                                                                       : ((t / 500) % 2 ? HIGH : LOW));
}
void serviceInputs() {
    updateLed();
    if (debouncedPress(BTN_LEFT_PIN, lastLeft))     send_button(0);
    if (debouncedPress(BTN_RIGHT_PIN, lastRight))   send_button(1);
    if (debouncedPress(BTN_SELECT_PIN, lastSelect)) send_button(2);

    // shutdown button: must be held SHUTDOWN_HOLD_MS (a bump does nothing)
    if (digitalRead(BTN_SHUTDOWN_PIN) == LOW) {
        if (shutdownDownSince == 0) shutdownDownSince = millis() ? millis() : 1;
        if (millis() - shutdownDownSince >= SHUTDOWN_HOLD_MS &&
            (lastShutdownSent == 0 || millis() - lastShutdownSent >= 1000)) {
            send_shutdown();                  // repeated each second while held, in case one is lost
            lastShutdownSent = millis() ? millis() : 1;
        }
    } else {
        shutdownDownSince = 0;
        lastShutdownSent = 0;
    }
}
inline void serviceDuringMove() {
    if (millis() - lastService >= 2) { lastService = millis(); serviceInputs(); }
}
void waitMs(unsigned long ms) {                // delay() that keeps the buttons alive
    unsigned long t0 = millis();
    while (millis() - t0 < ms) { serviceInputs(); delay(1); }
}

// ---------- Mechanics (unchanged from the validated sketch) ----------
void doSteps(long numSteps, int dirValue, unsigned int delayUs) {
    digitalWrite(DIR_PIN, dirValue);
    for (long i = 0; i < numSteps; i++) {
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(delayUs / 2);
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(delayUs / 2);
        serviceDuringMove();
    }
}

// Back off this far past the point where the switch releases before approaching it.
#define BACKOFF_MM        3.0f
// If the switch still reads pressed after moving this far away, it is stuck or miswired.
#define MAX_BACKOFF_MM   20.0f

void stepOnce(unsigned int delayUs) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(delayUs / 2);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(delayUs / 2);
    serviceDuringMove();
}

bool homeFailed(long stepsTaken) {
    homed = false;
    ledMode = LED_FAIL;
    send_debug(DBG_HOME, 0, (uint8_t)(stepsTaken >> 8), (uint8_t)stepsTaken);
    return false;
}

bool homeSlider() {
    unsigned long t0 = millis();
    ledMode = LED_BUSY;
    bool startPressed = limitPressedFiltered();
    Serial.printf("[%7lu] Homing (limit switch %s)...\n", t0,
                  startPressed ? "reads PRESSED, backing off to check it" : "open");

    // 1. If the switch already reads pressed, move AWAY until it releases.
    //    This handles a carriage really parked on the switch, and it catches a
    //    switch that falsely reads pressed (stuck, miswired, noise): instead of
    //    trusting a fake zero, homing fails loudly.
    if (startPressed) {
        long maxBack = (long)(MAX_BACKOFF_MM * STEPS_PER_MM);
        long backed = 0;
        digitalWrite(DIR_PIN, 1);
        while (limitPressedFiltered()) {
            stepOnce(HOMING_STEP_DELAY_US);
            if (++backed >= maxBack) {
                Serial.printf("[%7lu] HOMING FAILED: limit switch (GPIO%d) still reads PRESSED after backing off %.0f mm. "
                              "The switch is stuck, miswired, or GPIO%d is being pulled low. Check its wiring and ground.\n",
                              millis(), LIMIT_SWITCH_PIN, MAX_BACKOFF_MM, LIMIT_SWITCH_PIN);
                return homeFailed(0);
            }
        }
        doSteps((long)(BACKOFF_MM * STEPS_PER_MM), 1, HOMING_STEP_DELAY_US);   // clear it a little more
        Serial.printf("[%7lu] Switch released after %ld steps, now approaching it.\n", millis(), backed);
    }

    // 2. Approach the switch until it reads pressed (filtered).
    digitalWrite(DIR_PIN, 0);
    long stepsTaken = 0;
    while (!limitPressedFiltered()) {
        stepOnce(HOMING_STEP_DELAY_US);
        if (++stepsTaken >= MAX_HOMING_STEPS) {
            Serial.printf("[%7lu] HOMING FAILED: %ld steps without hitting the limit switch (GPIO%d). "
                          "Check the switch wiring and that the motor actually turned.\n", millis(), stepsTaken, LIMIT_SWITCH_PIN);
            return homeFailed(stepsTaken);
        }
    }
    currentPosition = 0;
    homed = true;
    ledMode = LED_OK;
    Serial.printf("[%7lu] Homed after %ld steps (%lu ms).\n", millis(), stepsTaken, millis() - t0);
    send_debug(DBG_HOME, 1, (uint8_t)(stepsTaken >> 8), (uint8_t)stepsTaken);
    return true;
}

// TMC2209 power-up: on a cold start (battery switch) the driver's logic and
// motor supplies come up at slightly different times, and an enable it sees
// during that window doesn't take: it ignores step pulses until EN is toggled.
// Pressing the ESP's EN button did that toggle by accident. This does it on purpose.
void armDriver() {
    digitalWrite(EN_PIN, HIGH);   // disable
    waitMs(100);
    digitalWrite(EN_PIN, LOW);    // enable (TMC2209 EN is active low)
    waitMs(100);
}

// Survives a software restart (not a power cut): how many times this power-up
// has already restarted itself because boot homing failed.
RTC_NOINIT_ATTR uint32_t bootRestarts;
#define MAX_BOOT_RESTARTS 2

// Home, and if it fails, re-arm the driver and try again.
#define HOME_ATTEMPTS 3
bool homeWithRetry() {
    for (int attempt = 1; attempt <= HOME_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            Serial.printf("[%7lu] Homing attempt %d of %d: re-arming the stepper driver first\n",
                          millis(), attempt, HOME_ATTEMPTS);
            armDriver();
            waitMs(400);
        }
        if (homeSlider()) return true;
    }
    return false;
}

void moveToColumn(int col) {
    col = constrain(col, 0, NUM_COLUMNS - 1);
    long diff = columnSteps[col] - currentPosition;
    if (diff != 0) {
        unsigned long t0 = millis();
        Serial.printf("[%7lu] Move to column %d: %ld steps %s\n", t0, col, labs(diff), diff > 0 ? "away from home" : "toward home");
        send_debug(DBG_MOVE, (uint8_t)col, 0);
        doSteps(labs(diff), (diff > 0) ? 1 : 0, STEP_DELAY_US);
        currentPosition = columnSteps[col];
        Serial.printf("[%7lu] At column %d (%lu ms)\n", millis(), col, millis() - t0);
        send_debug(DBG_MOVE, (uint8_t)col, 1);
    }
}

void servoTo(int angle) {
    angle = constrain(angle, 0, 180);
    if (!dispenserServo.attached()) dispenserServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
    dispenserServo.write(angle);
    servoNow = angle;
}

void dropPiece(uint8_t color) {
    int selectAngle = (color == 0) ? cal.redSel : cal.blackSel;
    int pushAngle   = (color == 0) ? cal.redPush : cal.blackPush;
    if (!dispenserServo.attached()) dispenserServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
    dispenserServo.write(selectAngle);
    waitMs(cal.waitMs);
    dispenserServo.write(pushAngle);
    waitMs(cal.waitMs);
    dispenserServo.write(cal.rest);
    waitMs(cal.waitMs);
    servoNow = cal.rest;
    Serial.printf("[%7lu] Servo drop done (%s)\n", millis(), color == 0 ? "RED" : "BLACK");
    send_debug(DBG_SERVO, color);
}

extern bool bootHomePending, bootHomeOk;
void handle_home_command() {
    if (homeWithRetry()) { bootHomePending = false; bootHomeOk = true; send_ready(); }
    else                 send_error(ERR_STEPPER_STALL);
}

void handle_drop_command(uint8_t col, uint8_t color) {
    if (!homed)                { Serial.println("  refused: not homed"); send_error(ERR_NOT_HOMED); return; }
    if (col >= NUM_COLUMNS || color > 1) { Serial.println("  refused: bad column/color"); send_error(ERR_INVALID_CMD); return; }

    moveToColumn(col);
    dropPiece(color);

    // Re-home after EVERY drop: step counting can't detect a missed step, so
    // each move starts from a freshly verified zero. The piece has already
    // dropped; if re-homing fails position tracking is untrustworthy, so
    // report an error rather than a DONE.
    if (homeWithRetry()) send_done(col);
    else              send_error(ERR_STEPPER_STALL);
}

// ---------- USB serial console (test the gantry without the Pi) ----------
void printStatus() {
    Serial.printf("[%7lu] STATUS fw v%d | homed=%d pos=%ld limit=%s | from Pi: %lu bytes, %lu good frames, %lu bad%s | to Pi: %lu frames | last reset: %s\n",
                  millis(), FW_BUILD, homed ? 1 : 0, currentPosition, limitPressed() ? "PRESSED" : "open",
                  rxBytes, rxFrames, (unsigned long)parser.bad_frames,
                  rxBytes == 0 ? "  <-- NOTHING received from the Pi: check Pi pin 8 (TX) -> GPIO14 and ground" : "",
                  txFrames, resetName(bootReason));
}
void printHelp() {
    Serial.println("Commands (type then Enter):");
    Serial.println("  s        status line now");
    Serial.println("  h        home the gantry");
    Serial.println("  m3       move to column 3 (0-6)");
    Serial.println("  d3 / b3  drop a RED / BLACK piece in column 3 (moves, drops, re-homes)");
    Serial.println("  l        watch the limit switch for 5 s");
    Serial.println("  p        send a test READY to the Pi");
    Serial.println("  r        restart this board");
    Serial.println("  cal      servo calibration commands");
}

// ---------- Servo calibration (serial monitor) ----------
void printCal() {
    char now[16];
    if (servoNow >= 0) snprintf(now, sizeof now, "%d deg", servoNow); else snprintf(now, sizeof now, "off/raw");
    Serial.printf("Servo now: %s | rest=%d  red: select=%d push=%d  black: select=%d push=%d  wait=%d ms\n",
                  now,
                  cal.rest, cal.redSel, cal.redPush, cal.blackSel, cal.blackPush, cal.waitMs);
}
void printCalHelp() {
    Serial.println("Servo calibration (type then Enter):");
    Serial.println("  sv           show the current servo values");
    Serial.println("  a90          move the servo to 90 deg and hold (0-180)");
    Serial.println("  + / -        nudge 1 deg     ++ / --   nudge 5 deg");
    Serial.println("  u1500        raw pulse in microseconds (500-2500), to find the end stops");
    Serial.println("  off          stop the pulses (servo goes limp)");
    Serial.println("  rest         store the CURRENT angle as REST (both silos blocked)");
    Serial.println("  red          store the current angle as RED select");
    Serial.println("  redpush      store the current angle as RED push");
    Serial.println("  black        store the current angle as BLACK select");
    Serial.println("  blackpush    store the current angle as BLACK push");
    Serial.println("  wait300      time to wait after each servo move, ms (bigger servo = slower)");
    Serial.println("  tr / tb      test a RED / BLACK drop right here (gantry does not move)");
    Serial.println("  save         keep these values after power-off (flash)");
    Serial.println("  defaults     go back to the built-in values (then save to keep)");
}
void loadCal() {
    prefs.begin("servo", true);
    cal.rest      = prefs.getInt("rest",      SERVO_DEFAULTS.rest);
    cal.redSel    = prefs.getInt("redSel",    SERVO_DEFAULTS.redSel);
    cal.redPush   = prefs.getInt("redPush",   SERVO_DEFAULTS.redPush);
    cal.blackSel  = prefs.getInt("blackSel",  SERVO_DEFAULTS.blackSel);
    cal.blackPush = prefs.getInt("blackPush", SERVO_DEFAULTS.blackPush);
    cal.waitMs    = prefs.getInt("waitMs",    SERVO_DEFAULTS.waitMs);
    prefs.end();
}
void saveCal() {
    prefs.begin("servo", false);
    prefs.putInt("rest", cal.rest);           prefs.putInt("redSel", cal.redSel);
    prefs.putInt("redPush", cal.redPush);     prefs.putInt("blackSel", cal.blackSel);
    prefs.putInt("blackPush", cal.blackPush); prefs.putInt("waitMs", cal.waitMs);
    prefs.end();
    Serial.println("Saved to flash. To also make them the built-in defaults, put this line in main.cpp:");
    Serial.printf("  const ServoCal SERVO_DEFAULTS = { %d, %d, %d, %d, %d, %d };\n",
                  cal.rest, cal.redSel, cal.redPush, cal.blackSel, cal.blackPush, cal.waitMs);
}
bool needAngle() {
    if (servoNow >= 0) return true;
    Serial.println("Move the servo with a<angle> first (e.g. a90), then store it.");
    return false;
}
// Returns true if `c` was a calibration command.
bool runCalCommand(const char* c) {
    if (!strcmp(c, "cal"))   { printCalHelp(); printCal(); return true; }
    if (!strcmp(c, "sv"))    { printCal(); return true; }
    if (!strcmp(c, "off"))   { dispenserServo.detach(); servoNow = -1; Serial.println("Servo off (limp)."); return true; }
    if (!strcmp(c, "+") || !strcmp(c, "-") || !strcmp(c, "++") || !strcmp(c, "--")) {
        if (!needAngle()) return true;
        int d = (c[0] == '+' ? 1 : -1) * (c[1] ? 5 : 1);
        servoTo(servoNow + d); printCal(); return true;
    }
    if (c[0] == 'a' && isdigit((unsigned char)c[1])) { servoTo(atoi(c + 1)); printCal(); return true; }
    if (c[0] == 'u' && isdigit((unsigned char)c[1])) {
        int us = constrain(atoi(c + 1), SERVO_MIN_US, SERVO_MAX_US);
        if (!dispenserServo.attached()) dispenserServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
        dispenserServo.writeMicroseconds(us);
        servoNow = -1;
        Serial.printf("Pulse %d us (about %d deg)\n", us, (int)((long)(us - SERVO_MIN_US) * 180 / (SERVO_MAX_US - SERVO_MIN_US)));
        return true;
    }
    if (!strncmp(c, "wait", 4) && isdigit((unsigned char)c[4])) { cal.waitMs = constrain(atoi(c + 4), 50, 3000); printCal(); return true; }
    struct { const char* name; int* slot; } slots[] = {
        { "rest", &cal.rest }, { "red", &cal.redSel }, { "redpush", &cal.redPush },
        { "black", &cal.blackSel }, { "blackpush", &cal.blackPush } };
    for (auto& sl : slots) if (!strcmp(c, sl.name)) {
        if (needAngle()) { *sl.slot = servoNow; Serial.printf("%s = %d deg (not saved yet: type save)\n", sl.name, servoNow); printCal(); }
        return true;
    }
    if (!strcmp(c, "tr") || !strcmp(c, "tb")) {
        Serial.printf("Test %s drop at the current gantry position\n", c[1] == 'r' ? "RED" : "BLACK");
        dropPiece(c[1] == 'r' ? 0 : 1); printCal(); return true;
    }
    if (!strcmp(c, "save"))     { saveCal(); return true; }
    if (!strcmp(c, "defaults")) { cal = SERVO_DEFAULTS; servoTo(cal.rest); Serial.println("Built-in values restored (not saved yet)."); printCal(); return true; }
    return false;
}
char consoleBuf[16]; int consoleLen = 0;
void runConsole(const char* c) {
    if (runCalCommand(c)) return;
    int n = (c[1] >= '0' && c[1] <= '9') ? c[1] - '0' : -1;
    switch (c[0]) {
        case '?': printHelp(); break;
        case 's': printStatus(); break;
        case 'h': homeSlider(); break;
        case 'm': if (n >= 0 && n < NUM_COLUMNS && homed) moveToColumn(n); else Serial.println("usage: m0..m6 (home first)"); break;
        case 'd': case 'b':
            if (n >= 0 && n < NUM_COLUMNS && homed) { moveToColumn(n); dropPiece(c[0] == 'd' ? 0 : 1); homeSlider(); }
            else Serial.println("usage: d0..d6 or b0..b6 (home first)");
            break;
        case 'l': {
            Serial.println("Limit switch for 5 s (press it by hand):");
            int last = -1; unsigned long t0 = millis();
            while (millis() - t0 < 5000) { int v = limitPressed(); if (v != last) { Serial.println(v ? "  PRESSED" : "  open"); last = v; } delay(10); }
            break;
        }
        case 'p': send_ready(); break;
        case 'r': if (c[1]) { Serial.println("Unknown command, type ? for help"); break; }
                  Serial.println("Restarting..."); delay(50); ESP.restart(); break;
        default: Serial.println("Unknown command, type ? for help"); break;
    }
}
void pollConsole() {
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            if (consoleLen > 0) { consoleBuf[consoleLen] = 0; runConsole(consoleBuf); consoleLen = 0; }
        } else if (consoleLen < (int)sizeof(consoleBuf) - 1) consoleBuf[consoleLen++] = ch;
    }
}

// After a failed boot homing the board keeps trying by itself (see loop()).
bool bootHomePending = false;
unsigned long lastAutoHome = 0;
int autoHomeTries = 0;
#define AUTO_HOME_EVERY_MS 3000
#define AUTO_HOME_MAX_TRIES 60

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);              // first thing: LED on = this program is running
    Serial.begin(115200);
    Serial2.begin(PI_UART_BAUD, SERIAL_8N1, PI_UART_RX, PI_UART_TX);
    bootReason = (uint8_t)esp_reset_reason();
    Serial.printf("\n=== Motion Controller fw v%d === reset reason: %s\n", FW_BUILD, resetName(bootReason));
    Serial.printf("Column 0 at %.1f mm, spacing %.1f mm, %.2f steps/mm\n", FIRST_COLUMN_OFFSET_MM, COLUMN_SPACING_MM, STEPS_PER_MM);
    Serial.printf("UART to Pi: RX GPIO%d (from Pi pin 8), TX GPIO%d (to Pi pin 10), %d baud. Type ? for test commands.\n",
                  PI_UART_RX, PI_UART_TX, PI_UART_BAUD);
    send_debug(DBG_BOOT, bootReason, FW_BUILD);

    pinMode(STEP_PIN, OUTPUT);
    pinMode(DIR_PIN, OUTPUT);
    pinMode(EN_PIN, OUTPUT);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
    pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
    pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
    pinMode(BTN_SHUTDOWN_PIN, INPUT_PULLUP);
    lastLeft = digitalRead(BTN_LEFT_PIN);
    lastRight = digitalRead(BTN_RIGHT_PIN);
    lastSelect = digitalRead(BTN_SELECT_PIN);

    digitalWrite(EN_PIN, HIGH);   // driver stays disabled until the power has settled
    for (int i = 0; i < NUM_COLUMNS; i++)
        columnSteps[i] = (long)((FIRST_COLUMN_OFFSET_MM + i * COLUMN_SPACING_MM) * STEPS_PER_MM + 0.5f);

    loadCal();
    Serial.printf("Servo: rest=%d red=%d/%d black=%d/%d wait=%d ms (type cal to calibrate)\n",
                  cal.rest, cal.redSel, cal.redPush, cal.blackSel, cal.blackPush, cal.waitMs);
    servoTo(cal.rest);

    // On battery everything powers up at once (Pi, screen, servo, driver).
    // Let the supplies settle, then re-arm the driver before the first move.
    if (bootReason != 3) bootRestarts = 0;     // a real power-up / EN press: start counting again
    waitMs(1500);
    armDriver();

    bootHomeOk = homeWithRetry();
    if (!bootHomeOk && bootRestarts < MAX_BOOT_RESTARTS) {
        // Pressing EN fixed this on a cold start; do the same thing ourselves.
        bootRestarts++;
        Serial.printf("[%7lu] Boot homing failed: restarting this board (%u of %d), same as pressing EN\n",
                      millis(), (unsigned)bootRestarts, MAX_BOOT_RESTARTS);
        Serial.flush(); Serial2.flush();
        delay(200);
        ESP.restart();
        return;                               // (only reached in the host tests)
    }
    if (bootHomeOk) bootRestarts = 0;
    bootHomePending = !bootHomeOk;            // loop() keeps retrying every few seconds
    lastAutoHome = millis();
    Serial.println(bootHomeOk ? "Boot homing OK. Beaconing CMD_READY until the Pi answers."
                              : "Boot homing FAILED. Beaconing CMD_ERROR and retrying every 3 s.");
    if (bootHomeOk) send_ready(); else send_error(ERR_STEPPER_STALL);
    lastBeacon = millis();
}

void loop() {
    // ---- commands from the Pi ----
    while (Serial2.available()) {
        rxBytes++;
        if (!parser.feed_byte(Serial2.read())) continue;
        rxFrames++;
        if (!heardPi) Serial.printf("[%7lu] First frame from the Pi: link works in both directions. Beacon off.\n", millis());
        heardPi = true;                       // stops the beacon permanently
        Serial.printf("[%7lu] RX <- Pi: %s", millis(), cmdName(parser.cmd));
        for (int i = 0; i < parser.payload_len; i++) Serial.printf(" %u", parser.payload[i]);
        Serial.println();
        send_debug(DBG_RX, parser.cmd, parser.payload_len > 0 ? parser.payload[0] : 0, parser.payload_len > 1 ? parser.payload[1] : 0);
        switch (parser.cmd) {
            case CMD_HOME:     handle_home_command(); break;
            case CMD_DROP:     if (parser.payload_len >= 2) handle_drop_command(parser.payload[0], parser.payload[1]);
                               else send_error(ERR_INVALID_CMD);
                               break;
            case CMD_SELECTOR: if (homed && parser.payload_len >= 1) moveToColumn(parser.payload[0]);
                               else if (!homed) Serial.println("  ignored: not homed");
                               break;
            case CMD_PING:     send_status(); break;
            case CMD_RESET:    Serial.println("  Pi asked for a restart"); Serial2.flush(); delay(50); ESP.restart(); break;
            default: break;                   // retired / unknown commands are ignored
        }
    }

    // ---- corrupted frames from the Pi (noise, wrong baud, loose wire) ----
    if (parser.bad_frames != lastBadReported && millis() - lastBadSent > 500) {
        Serial.printf("[%7lu] BAD FRAME(S) from the Pi: %lu total (noise, loose wire or baud mismatch)\n", millis(), (unsigned long)parser.bad_frames);
        send_debug(DBG_BAD, (uint8_t)(parser.bad_frames >> 8), (uint8_t)parser.bad_frames);
        lastBadReported = parser.bad_frames; lastBadSent = millis();
    }

    pollConsole();
    if (millis() - lastStatusPrint >= STATUS_PRINT_MS) { printStatus(); lastStatusPrint = millis(); }

    // ---- buttons and the shutdown button: forwarded to the Pi in every state ----
    serviceInputs();

    // ---- boot homing failed: keep trying by itself, so nobody has to press EN ----
    if (bootHomePending && !homed && autoHomeTries < AUTO_HOME_MAX_TRIES && millis() - lastAutoHome >= AUTO_HOME_EVERY_MS) {
        autoHomeTries++;
        Serial.printf("[%7lu] Automatic homing retry %d of %d\n", millis(), autoHomeTries, AUTO_HOME_MAX_TRIES);
        armDriver();
        if (homeSlider()) {
            bootHomePending = false; bootHomeOk = true;
            Serial.println("Homing recovered. Sending CMD_READY.");
            send_ready();                     // the Pi goes back to its menu on this, even from the fault screen
        }
        lastAutoHome = millis();
    }
    if (homed) bootHomePending = false;

    // ---- beacon until the Pi has spoken ----
    if (!heardPi && millis() - lastBeacon > BEACON_INTERVAL_MS) {
        if (bootHomeOk) send_ready(); else send_error(ERR_STEPPER_STALL);
        lastBeacon = millis();
    }
}