// ============================================================
// Motion Controller: Protocol-Driven Version
// ============================================================
// Stepper/servo mechanics (doSteps, homeSlider, moveToColumn, dropPiece)
// are carried over UNCHANGED from Finn's validated bench-test sketch --
// same pins, same calibration, same blocking step timing. What's new
// here is the UART layer: instead of local buttons deciding what to
// do, Game Brain's CMD_HOME/CMD_DROP commands do, and this board
// reports back CMD_READY/CMD_DONE/CMD_ERROR the way protocol.h expects.
//
// This board still knows nothing about Connect Four -- it only knows
// "go to column N, drop color C," exactly as before.
#include <Arduino.h>
#include <ESP32Servo.h>
#include "protocol.h"

// ---------- Pins (confirmed working, from the validated test rig) ----------
#define STEP_PIN         25
#define DIR_PIN          26
#define EN_PIN           27
#define LIMIT_SWITCH_PIN 33
#define SERVO_PIN        32

// Set to 0 to fully remove the manual jog buttons (e.g. if these GPIOs
// are needed for something else later). Left on by default so the
// bench-test diagnostic capability survives alongside real UART control.
#define ENABLE_MANUAL_BUTTONS 1
#if ENABLE_MANUAL_BUTTONS
#define BTN_LEFT_PIN   21
#define BTN_RIGHT_PIN  23
#define BTN_SELECT_PIN 22
#endif

#define MOTION_UART_BAUD 115200
#define MOTION_UART_RX   14
#define MOTION_UART_TX   2   // shares the onboard LED on most ESP32 dev
                                // boards -- it'll flicker with UART
                                // traffic, which is cosmetic only. See
                                // note below on why this pin specifically
                                // is safe to use despite being a "boot
                                // strapping" pin.

// ---------- Motion constants -- measured values, not guesses ----------
const float STEPS_PER_MM = 39.46;       // measured: 3200 commanded steps = 81.1mm actual travel
const float COLUMN_SPACING_MM = 33.3;   // 32mm column + 1.3mm wall, measured center-to-center
const int NUM_COLUMNS = 7;
long columnSteps[NUM_COLUMNS];

const unsigned int STEP_DELAY_US = 500;
const unsigned int HOMING_STEP_DELAY_US = 800;

// ---------- Servo angles ----------
// color==0 -> RED (PLAYER_RED in protocol.h) -> "Color A" angle
// color==1 -> BLACK (PLAYER_BLACK) -> "Color B" angle
// ASSUMPTION carried over from the bench-test naming -- verify physically
// that the silo aligned with SERVO_COLOR_A is actually loaded with red
// tokens, not black, before trusting this mapping in a real game.
const int SERVO_REST = 90;
const int SERVO_COLOR_A = 60;
const int SERVO_COLOR_B = 120;
const int SERVO_PUSH_OFFSET = 15;

Servo dispenserServo;
FrameParser parser;

long currentPosition = 0;
int currentCol = 0;

#if ENABLE_MANUAL_BUTTONS
int lastLeft, lastRight, lastSelect;
uint8_t manualNextColor = 0;
#endif

// ------------------------------------------------------------
// Mechanics -- unchanged from the validated bench-test sketch
// ------------------------------------------------------------
void doSteps(long numSteps, int dirValue, unsigned int delayUs) {
    digitalWrite(DIR_PIN, dirValue);
    for (long i = 0; i < numSteps; i++) {
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(delayUs / 2);
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(delayUs / 2);
    }
}

void homeSlider() {
    Serial.println("Homing...");
    digitalWrite(DIR_PIN, 0); // confirmed working in the bench test,
                                // carried over as-is
    while (digitalRead(LIMIT_SWITCH_PIN) == HIGH) { // idle high, pressed pulls low
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(HOMING_STEP_DELAY_US / 2);
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(HOMING_STEP_DELAY_US / 2);
    }
    currentPosition = 0;
    Serial.println("Homed.");
}

void moveToColumn(int col) {
    col = constrain(col, 0, NUM_COLUMNS - 1);
    long target = columnSteps[col];
    long diff = target - currentPosition;
    if (diff != 0) {
        int dirValue = (diff > 0) ? 1 : 0;
        doSteps(abs(diff), dirValue, STEP_DELAY_US);
        currentPosition = target;
    }
    currentCol = col;
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

#if ENABLE_MANUAL_BUTTONS
bool debouncedPress(int pin, int &lastState) {
    int state = digitalRead(pin);
    bool pressed = (lastState == HIGH && state == LOW);
    lastState = state;
    return pressed;
}
#endif

// ------------------------------------------------------------
// Protocol integration -- new
// ------------------------------------------------------------
void send_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t flen = build_frame(cmd, payload, len, frame);
    Serial2.write(frame, flen);
}
void send_ready() { send_frame(CMD_READY, nullptr, 0); }
void send_done(uint8_t col) { uint8_t p[1] = { col }; send_frame(CMD_DONE, p, 1); }
void send_error(uint8_t code) { uint8_t p[1] = { code }; send_frame(CMD_ERROR, p, 1); }

void handle_drop_command(uint8_t col, uint8_t color) {
    Serial.print("Received CMD_DROP: col=");
    Serial.print(col);
    Serial.print(" color=");
    Serial.println(color);

    if (col >= NUM_COLUMNS) {
        // Protocol-level fault: Game Brain should never send an
        // out-of-range column, since it tracks board state itself --
        // if this happens, something is desynced between the boards.
        Serial.println("ERROR: invalid column received");
        send_error(ERR_INVALID_CMD);
        return;
    }
    moveToColumn(col);
    dropPiece(color);
    send_done(col); // sent only after both the move and the drop have
                      // actually completed -- Game Brain won't advance
                      // its board state until it sees this
    Serial.println("Sent CMD_DONE");
}

void setup() {
    Serial.begin(115200); // USB debug only
    Serial2.begin(MOTION_UART_BAUD, SERIAL_8N1, MOTION_UART_RX, MOTION_UART_TX);

    pinMode(STEP_PIN, OUTPUT);
    pinMode(DIR_PIN, OUTPUT);
    pinMode(EN_PIN, OUTPUT);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
#if ENABLE_MANUAL_BUTTONS
    pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
    pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
    pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
#endif

    digitalWrite(EN_PIN, LOW); // TMC2209 EN is active low

    for (int i = 0; i < NUM_COLUMNS; i++) {
        columnSteps[i] = (long)(i * COLUMN_SPACING_MM * STEPS_PER_MM);
    }

    dispenserServo.attach(SERVO_PIN, 500, 2500);
    dispenserServo.write(SERVO_REST);

    homeSlider();
    send_ready(); // matches protocol.h: Game Brain waits for this
                    // before ever sending a CMD_DROP
    Serial.println("Sent CMD_READY");

#if ENABLE_MANUAL_BUTTONS
    lastLeft = digitalRead(BTN_LEFT_PIN);
    lastRight = digitalRead(BTN_RIGHT_PIN);
    lastSelect = digitalRead(BTN_SELECT_PIN);
#endif
}

void loop() {
    // ---- Primary control path: commands from Game Brain over UART ----
    while (Serial2.available()) {
        if (parser.feed_byte(Serial2.read())) {
            if (parser.cmd == CMD_HOME) {
                homeSlider();
                send_ready();
                Serial.println("Sent CMD_READY (re-homed)");
            } else if (parser.cmd == CMD_DROP) {
                handle_drop_command(parser.payload[0], parser.payload[1]);
            }
        }
    }

#if ENABLE_MANUAL_BUTTONS
    // ---- Optional manual/diagnostic override -- bypasses Game Brain ----
    // Useful for bench testing the mechanism on its own, same as the
    // original validated sketch. Safe to leave enabled alongside UART
    // control since nothing here interferes with the protocol handling
    // above -- but a manual jog moves currentCol/currentPosition out
    // from under whatever Game Brain thinks the state is, so avoid
    // using this mid-game.
    if (debouncedPress(BTN_LEFT_PIN, lastLeft)) moveToColumn(currentCol - 1);
    if (debouncedPress(BTN_RIGHT_PIN, lastRight)) moveToColumn(currentCol + 1);
    if (debouncedPress(BTN_SELECT_PIN, lastSelect)) {
        dropPiece(manualNextColor);
        manualNextColor = 1 - manualNextColor;
    }
    delay(20); // matches the original sketch's debounce interval
#endif
}