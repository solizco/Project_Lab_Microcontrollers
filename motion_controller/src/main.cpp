// ============================================================
// Motion Controller (ESP32-WROOM-32E): MOTION ONLY
// ============================================================
// The Raspberry Pi owns the game, the menu and the display. This board:
//   - homes the gantry (limit switch) and reports CMD_READY
//   - moves to a column for CMD_SELECTOR (live tracking of the human's choice)
//   - executes CMD_DROP: move, servo drop, RE-HOME, then CMD_DONE (or CMD_ERROR)
//   - forwards the three buttons to the Pi as CMD_BUTTON, in EVERY state
// It knows nothing about Connect Four rules or what is on screen.
//
// Retired commands (CMD_FACE / CMD_MENU / CMD_FAULT / CMD_MODE_SELECTED)
// are ignored if they ever arrive.
#include <Arduino.h>
#include <ESP32Servo.h>
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

// UART to the Pi: Pi GPIO14 (TX) -> GPIO14 here (RX); Pi GPIO15 (RX) <- GPIO5 here (TX).
// GPIO2 fights the bootloader and GPIO34/35/36/39 are input-only, so TX is GPIO5.
#define PI_UART_BAUD 115200
#define PI_UART_RX   14
#define PI_UART_TX   5

// ---------- Motion constants (measured, unchanged) ----------
const float STEPS_PER_MM = 39.46;
const float COLUMN_SPACING_MM = 33.3;
const int NUM_COLUMNS = 7;
long columnSteps[NUM_COLUMNS];

const unsigned int STEP_DELAY_US = 500;
const unsigned int HOMING_STEP_DELAY_US = 800;
#define MAX_HOMING_STEPS 20000

// color 0 (RED) -> "Color A" angle; color 1 (BLACK) -> "Color B"
const int SERVO_REST = 90;
const int SERVO_COLOR_A = 60;
const int SERVO_COLOR_B = 120;
const int SERVO_PUSH_OFFSET = 15;

Servo dispenserServo;
FrameParser parser;
long currentPosition = 0;
bool homed = false;

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
}
void send_ready()           { send_frame(CMD_READY, nullptr, 0); }
void send_done(uint8_t col) { uint8_t p[1] = { col };  send_frame(CMD_DONE, p, 1); }
void send_error(uint8_t c)  { uint8_t p[1] = { c };    send_frame(CMD_ERROR, p, 1); }
void send_button(uint8_t b) { uint8_t p[1] = { b };    send_frame(CMD_BUTTON, p, 1);
                              Serial.print("SENT: CMD_BUTTON "); Serial.println(b); }

// ---------- Mechanics (unchanged from the validated sketch) ----------
void doSteps(long numSteps, int dirValue, unsigned int delayUs) {
    digitalWrite(DIR_PIN, dirValue);
    for (long i = 0; i < numSteps; i++) {
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(delayUs / 2);
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(delayUs / 2);
    }
}

bool homeSlider() {
    Serial.println("Homing...");
    digitalWrite(DIR_PIN, 0);
    long stepsTaken = 0;
    while (digitalRead(LIMIT_SWITCH_PIN) == HIGH) {
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(HOMING_STEP_DELAY_US / 2);
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(HOMING_STEP_DELAY_US / 2);
        stepsTaken++;
        if (stepsTaken >= MAX_HOMING_STEPS) {
            Serial.println("HOMING FAILED: max travel without hitting the limit switch.");
            homed = false;
            return false;
        }
    }
    currentPosition = 0;
    homed = true;
    Serial.println("Homed.");
    return true;
}

void moveToColumn(int col) {
    col = constrain(col, 0, NUM_COLUMNS - 1);
    long diff = columnSteps[col] - currentPosition;
    if (diff != 0) {
        doSteps(abs(diff), (diff > 0) ? 1 : 0, STEP_DELAY_US);
        currentPosition = columnSteps[col];
    }
}

void dropPiece(uint8_t color) {
    int selectAngle = (color == 0) ? SERVO_COLOR_A : SERVO_COLOR_B;
    int pushAngle = selectAngle + ((color == 0) ? SERVO_PUSH_OFFSET : -SERVO_PUSH_OFFSET);
    dispenserServo.write(selectAngle);
    delay(250);
    dispenserServo.write(pushAngle);
    delay(250);
    dispenserServo.write(SERVO_REST);
    delay(200);
}

void handle_home_command() {
    if (homeSlider()) { send_ready(); Serial.println("Sent CMD_READY"); }
    else              { send_error(ERR_STEPPER_STALL); Serial.println("Homing failed -> CMD_ERROR"); }
}

void handle_drop_command(uint8_t col, uint8_t color) {
    Serial.print("CMD_DROP col="); Serial.print(col); Serial.print(" color="); Serial.println(color);
    if (!homed)                { send_error(ERR_NOT_HOMED);   return; }
    if (col >= NUM_COLUMNS || color > 1) { send_error(ERR_INVALID_CMD); return; }

    moveToColumn(col);
    dropPiece(color);

    // Re-home after EVERY drop: step counting can't detect a missed step, so
    // each move starts from a freshly verified zero. The piece has already
    // dropped; if re-homing fails position tracking is untrustworthy, so
    // report an error rather than a DONE.
    if (homeSlider()) { send_done(col); Serial.println("Sent CMD_DONE"); }
    else              { send_error(ERR_STEPPER_STALL); Serial.println("Post-drop re-home failed -> CMD_ERROR"); }
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(PI_UART_BAUD, SERIAL_8N1, PI_UART_RX, PI_UART_TX);

    pinMode(STEP_PIN, OUTPUT);
    pinMode(DIR_PIN, OUTPUT);
    pinMode(EN_PIN, OUTPUT);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
    pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
    pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
    lastLeft = digitalRead(BTN_LEFT_PIN);
    lastRight = digitalRead(BTN_RIGHT_PIN);
    lastSelect = digitalRead(BTN_SELECT_PIN);

    digitalWrite(EN_PIN, LOW);
    for (int i = 0; i < NUM_COLUMNS; i++)
        columnSteps[i] = (long)(i * COLUMN_SPACING_MM * STEPS_PER_MM);

    dispenserServo.attach(SERVO_PIN, 500, 2500);
    dispenserServo.write(SERVO_REST);

    bootHomeOk = homeSlider();
    Serial.println(bootHomeOk ? "Boot homing OK. Beaconing CMD_READY until the Pi answers."
                              : "Boot homing FAILED. Beaconing CMD_ERROR until the Pi answers.");
    if (bootHomeOk) send_ready(); else send_error(ERR_STEPPER_STALL);
    lastBeacon = millis();
}

void loop() {
    // ---- commands from the Pi ----
    while (Serial2.available()) {
        if (!parser.feed_byte(Serial2.read())) continue;
        heardPi = true;                       // stops the beacon permanently
        switch (parser.cmd) {
            case CMD_HOME:     handle_home_command(); break;
            case CMD_DROP:     if (parser.payload_len >= 2) handle_drop_command(parser.payload[0], parser.payload[1]);
                               else send_error(ERR_INVALID_CMD);
                               break;
            case CMD_SELECTOR: if (homed && parser.payload_len >= 1) moveToColumn(parser.payload[0]); break;
            default: break;                   // retired / unknown commands are ignored
        }
    }

    // ---- buttons: forwarded to the Pi in every state, nothing handled locally ----
    if (debouncedPress(BTN_LEFT_PIN, lastLeft))     send_button(0);
    if (debouncedPress(BTN_RIGHT_PIN, lastRight))   send_button(1);
    if (debouncedPress(BTN_SELECT_PIN, lastSelect)) send_button(2);

    // ---- beacon until the Pi has spoken ----
    if (!heardPi && millis() - lastBeacon > BEACON_INTERVAL_MS) {
        if (bootHomeOk) send_ready(); else send_error(ERR_STEPPER_STALL);
        lastBeacon = millis();
    }
}