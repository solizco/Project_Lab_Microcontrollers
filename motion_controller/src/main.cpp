// ============================================================
// Motion Controller: Full Merge
// ============================================================
// Combines, unchanged in their own logic:
//   - Stepper/servo mechanics (Finn's validated bench-test sketch)
//   - Local menu UI (Finn's validated standalone menu sketch)
//   - Roger's face/clip player (Finn's validated clip-player sketch)
// plus the UART protocol layer tying all three to Game Brain.
//
// Screen ownership is exactly one of three mutually exclusive modes:
//   SCREEN_LOCAL_MENU -- active from boot through mode selection.
//                        Buttons navigate/confirm LOCALLY; nothing is
//                        sent to Game Brain until CONFIRM is pressed.
//   SCREEN_ROGER      -- active for the whole game once a mode is
//                        confirmed. Shows whichever face clip Game
//                        Brain last requested, with the column
//                        selector as a small overlay when present.
//                        Buttons are FORWARDED to Game Brain as
//                        CMD_BUTTON instead of doing anything locally.
//   SCREEN_FAULT      -- terminal, matches Game Brain's own FAULT
//                        state. Buttons do nothing; recovery is a
//                        physical power cycle.
#include <Arduino.h>
#include <ESP32Servo.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <JPEGDEC.h>
#include "protocol.h"

// ---------- Pins (confirmed working, from the validated test rigs) ----------
#define STEP_PIN         25
#define DIR_PIN          26
#define EN_PIN           27
#define LIMIT_SWITCH_PIN 33
#define SERVO_PIN        32

#define TFT_SCK   18
#define TFT_MOSI  19
#define TFT_DC    13
#define TFT_CS    16
#define TFT_RST   4
#define TFT_BLK   17   // set to -1 if the board has no backlight pin
#define GFX_IPS   true
#define SPI_HZ    40000000
#define SCREEN_W  320
#define SCREEN_H  240

#define BTN_LEFT_PIN   21
#define BTN_RIGHT_PIN  23
#define BTN_SELECT_PIN 22

#define MOTION_UART_BAUD 115200
#define MOTION_UART_RX   14
#define MOTION_UART_TX   5   // GPIO5: normal bidirectional pin, no
                                // boot-strapping role, no input-only
                                // restriction. (GPIO2 was dropped after
                                // it kept fighting the bootloader during
                                // flashing; GPIO35 doesn't work at all --
                                // it's one of the ESP32's input-only
                                // pins, 34/35/36/39, with no output
                                // driver on the silicon -- UART TX can
                                // never work there, on any board.)

// ---------- Motion constants -- measured values, not guesses ----------
const float STEPS_PER_MM = 39.46;
const float COLUMN_SPACING_MM = 33.3;
const int NUM_COLUMNS = 7;
long columnSteps[NUM_COLUMNS];

const unsigned int STEP_DELAY_US = 500;
const unsigned int HOMING_STEP_DELAY_US = 800;
#define MAX_HOMING_STEPS 20000

// color==0 -> RED (PLAYER_RED) -> "Color A" angle; color==1 -> BLACK -> "Color B"
const int SERVO_REST = 90;
const int SERVO_COLOR_A = 60;
const int SERVO_COLOR_B = 120;
const int SERVO_PUSH_OFFSET = 15;

Servo dispenserServo;
FrameParser parser;
long currentPosition = 0;
int currentCol = 0;

// ---------- Display ----------
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI,
                                            GFX_NOT_DEFINED, VSPI);
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RST, 1 /* landscape */, GFX_IPS, 240, 320);

#define COL_BG     BLACK
#define COL_FG     WHITE
#define COL_DIM    0x8410   // mid gray
#define COL_ACCENT 0x07FF   // cyan

void printCentered(const char *text, int y, uint8_t size, uint16_t color) {
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    int w = strlen(text) * 6 * size;
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    gfx->setCursor(x, y);
    gfx->print(text);
}

// ---------- Local menu (matches SelectedMode in protocol.h exactly) ----------
#define GAME_MODE_COUNT 4
const char *modeNames[GAME_MODE_COUNT] = { "PASS & PLAY", "NOOB", "PRO", "HACKER" };
const char *modeBlurb[GAME_MODE_COUNT] = {
    "two humans, no AI", "Roger plays loose", "Roger plays sharp", "Roger plays perfect"
};
int menuIndex = 0;

const int rowH = 34, rowGap = 8, rowX = 30, rowW = SCREEN_W - 2 * rowX, rowTop = 62;
int rowY(int i) { return rowTop + i * (rowH + rowGap); }

void drawMenuRow(int i, bool selected) {
    int y = rowY(i);
    gfx->fillRoundRect(rowX, y, rowW, rowH, 6, selected ? COL_ACCENT : COL_BG);
    gfx->drawRoundRect(rowX, y, rowW, rowH, 6, selected ? COL_ACCENT : COL_DIM);
    gfx->setTextSize(2);
    gfx->setTextColor(selected ? COL_BG : COL_FG);
    gfx->setCursor(rowX + 14, y + 10);
    gfx->print(modeNames[i]);
    if (selected) {
        int ax = rowX + rowW - 22, ay = y + rowH / 2;
        gfx->fillTriangle(ax, ay - 6, ax, ay + 6, ax + 8, ay, COL_BG);
    }
}

void drawMenuBlurb(int i) {
    int y = rowY(GAME_MODE_COUNT - 1) + rowH + 8;
    gfx->fillRect(0, y, SCREEN_W, 14, COL_BG);
    printCentered(modeBlurb[i], y + 3, 1, COL_DIM);
}

void drawMenu() {
    gfx->fillScreen(COL_BG);
    printCentered("CONNECT 4", 14, 3, COL_ACCENT);
    printCentered("with ROGER", 42, 1, COL_DIM);
    for (int i = 0; i < GAME_MODE_COUNT; i++) drawMenuRow(i, i == menuIndex);
    drawMenuBlurb(menuIndex);
    printCentered("L / R  choose     SELECT  start", SCREEN_H - 14, 1, COL_DIM);
}

void menuMove(int delta) {
    int old = menuIndex;
    int next = (menuIndex + delta + GAME_MODE_COUNT) % GAME_MODE_COUNT;
    if (next == old) return;
    menuIndex = next;
    drawMenuRow(old, false);
    drawMenuRow(next, true);
    drawMenuBlurb(next);
    Serial.print("Highlight: ");
    Serial.println(modeNames[next]);
}

// ---------- Roger's face clips ----------
enum ClipSlot { CLIP_IDLE, CLIP_GOOD_MOVE, CLIP_BAD_MOVE, CLIP_WIN, CLIP_LOSS, CLIP_COUNT };
#define MAX_VARIANTS 4
#define FRAME_BUF_SIZE 24576
#define CLIP_W 320
#define CLIP_H 240
const unsigned long frameInterval = 100; // 10 fps

struct Clip {
    const char *dir;
    bool loopForever;
    uint8_t variants;
    uint16_t frames[MAX_VARIANTS];
};

Clip clips[CLIP_COUNT] = {
    { "/idle",       true,  0, {0} },
    { "/good_move",  false, 0, {0} },
    { "/bad_move",   false, 0, {0} },
    { "/roger_win",  true,  0, {0} },
    { "/roger_loss", true,  0, {0} },
};

ClipSlot currentClip = CLIP_IDLE;
uint8_t currentVariant = 0;
uint16_t currentFrame = 0;
unsigned long lastFrameTime = 0;

JPEGDEC jpeg;
static uint8_t frameBuf[FRAME_BUF_SIZE];
const int clipX = (SCREEN_W - CLIP_W) / 2;
const int clipY = (SCREEN_H - CLIP_H) / 2;

int jpegDrawCallback(JPEGDRAW *pDraw) {
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    return 1;
}

uint16_t countFrames(const char *dir, uint8_t variant) {
    char path[64];
    uint16_t n = 0;
    while (n < 999) {
        snprintf(path, sizeof(path), "%s/%u/%03u.jpg", dir, variant, n);
        if (!LittleFS.exists(path)) break;
        n++;
    }
    return n;
}

bool drawClipFrame(const char *dir, uint8_t variant, uint16_t index) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%u/%03u.jpg", dir, variant, index);
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t len = f.size();
    if (len > FRAME_BUF_SIZE) { f.close(); return false; }
    f.read(frameBuf, len);
    f.close();
    if (!jpeg.openRAM(frameBuf, len, jpegDrawCallback)) return false;
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    jpeg.decode(clipX, clipY, 0);
    jpeg.close();
    return true;
}

// ---------- Column-selector overlay (drawn on top of the current clip) ----------
bool showSelector = false;
uint8_t selectorCol = 0;

void drawSelectorOverlay() {
    if (!showSelector) return;
    int boxW = SCREEN_W / NUM_COLUMNS;
    int y = SCREEN_H - 16;
    for (int i = 0; i < NUM_COLUMNS; i++) {
        int x = i * boxW + boxW / 2;
        uint16_t color = (i == selectorCol) ? COL_ACCENT : COL_DIM;
        gfx->fillCircle(x, y, 6, color);
    }
}

void setClip(ClipSlot slot) {
    if (clips[slot].variants == 0) return; // no clips found for this slot
                                              // (e.g. LittleFS didn't mount) --
                                              // stay on whatever was showing
                                              // rather than crash
    currentClip = slot;
    currentVariant = random(clips[slot].variants);
    currentFrame = 0;
    lastFrameTime = 0;
}

ClipSlot face_event_to_clip(FaceEvent evt) {
    switch (evt) {
        case FACE_IDLE:      return CLIP_IDLE;
        case FACE_THINKING:  return CLIP_IDLE; // PLACEHOLDER: no dedicated
                                                  // "thinking" clip exists
                                                  // yet -- add a CLIP_THINKING
                                                  // slot + data/thinking/
                                                  // folder once that art exists
        case FACE_GOOD_MOVE: return CLIP_GOOD_MOVE;
        case FACE_BAD_MOVE:  return CLIP_BAD_MOVE;
        case FACE_WIN:       return CLIP_WIN;
        case FACE_LOSS:      return CLIP_LOSS;
    }
    return CLIP_IDLE;
}

void advanceClipAnimation() {
    Clip &c = clips[currentClip];

    if (c.variants == 0) {
        // No clip loaded for this slot -- either the filesystem image
        // was never uploaded, or this specific clip never got
        // converted from its source GIF into the JPG-frame-sequence
        // format this code actually reads. Draw a visible placeholder
        // instead of silently leaving whatever was on screen before --
        // that's what was making a real screenMode change look like
        // nothing happened at all.
        static ClipSlot lastDrawnPlaceholder = CLIP_COUNT; // forces first draw
        if (currentClip != lastDrawnPlaceholder) {
            const char* names[CLIP_COUNT] = { "IDLE", "GOOD MOVE", "BAD MOVE", "WIN", "LOSS" };
            gfx->fillScreen(COL_BG);
            printCentered(names[currentClip], SCREEN_H / 2 - 10, 2, COL_ACCENT);
            printCentered("(clip not loaded)", SCREEN_H / 2 + 20, 1, COL_DIM);
            drawSelectorOverlay();
            lastDrawnPlaceholder = currentClip;
        }
        return;
    }

    unsigned long now = millis();
    if (now - lastFrameTime < frameInterval) return;
    lastFrameTime = now;

    drawClipFrame(c.dir, currentVariant, currentFrame);
    drawSelectorOverlay();

    currentFrame++;
    if (currentFrame >= c.frames[currentVariant]) {
        if (c.loopForever) {
            currentFrame = 0;
        } else {
            setClip(CLIP_IDLE); // one-shot reactions fall back to idle
        }
    }
}

// ---------- Fault screen ----------
void drawFaultScreen(uint8_t error_code) {
    gfx->fillScreen(RED);
    gfx->setTextSize(2);
    gfx->setTextColor(WHITE);
    gfx->setCursor(10, 60);
    gfx->print("MOTION FAULT");
    gfx->setTextSize(1);
    gfx->setCursor(10, 100);
    gfx->print("Error code: ");
    gfx->print(error_code);
    gfx->setCursor(10, 120);
    gfx->print("Power-cycle to reset.");
}

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

// ---------- Screen mode ----------
enum ScreenMode { SCREEN_LOCAL_MENU, SCREEN_LOCAL_CONFIRM, SCREEN_ROGER, SCREEN_FAULT };
ScreenMode screenMode = SCREEN_LOCAL_MENU;

// ---------- CMD_READY beacon ----------
// CMD_READY used to be one-shot: sent exactly once after boot-time
// homing, never again unless Game Brain explicitly asked for a fresh
// CMD_HOME. If that single message got missed (Game Brain not quite
// listening yet, a UART hiccup), the only recovery was Game Brain's
// own retry timer -- which also forces a full, physically unnecessary
// re-home each time, even though Motion Controller is already sitting
// there correctly homed and idle. Re-broadcasting periodically while
// on the local menu is safe (Game Brain ignores CMD_READY outside its
// own HOMING state) and lets the handshake complete in a couple of
// seconds in almost all cases instead of relying on a slow fallback.
unsigned long last_ready_beacon = 0;
#define READY_BEACON_INTERVAL_MS 2000

// ---------- Confirm screen: "You have selected X -- Start Game / Back" ----------
int confirmIndex = 0; // 0 = Start Game, 1 = Back to Selection
const char* confirmOptions[2] = { "START GAME", "BACK TO SELECTION" };

int confirmOptionY(int i) {
    int boxY = 38, boxH = 44;
    int blurbY = boxY + boxH + 10;
    int firstOptY = blurbY + 20;
    return firstOptY + i * (rowH + rowGap);
}

void drawConfirmOption(int i, bool selected) {
    int y = confirmOptionY(i);
    gfx->fillRoundRect(rowX, y, rowW, rowH, 6, selected ? COL_ACCENT : COL_BG);
    gfx->drawRoundRect(rowX, y, rowW, rowH, 6, selected ? COL_ACCENT : COL_DIM);
    gfx->setTextSize(2);
    gfx->setTextColor(selected ? COL_BG : COL_FG);
    gfx->setCursor(rowX + 14, y + 10);
    gfx->print(confirmOptions[i]);
}

void drawConfirm() {
    gfx->fillScreen(COL_BG);
    printCentered("MODE SELECTED", 10, 2, COL_DIM);

    const char *name = modeNames[menuIndex];
    int textW = strlen(name) * 6 * 3;
    int boxW = textW + 40, boxH = 44;
    int boxX = (SCREEN_W - boxW) / 2, boxY = 38;
    gfx->fillRoundRect(boxX, boxY, boxW, boxH, 8, COL_ACCENT);
    gfx->setTextSize(3);
    gfx->setTextColor(COL_BG);
    gfx->setCursor(boxX + 20, boxY + 11);
    gfx->print(name);

    printCentered(modeBlurb[menuIndex], boxY + boxH + 10, 1, COL_FG);

    drawConfirmOption(0, confirmIndex == 0);
    drawConfirmOption(1, confirmIndex == 1);

    printCentered("L / R  choose     SELECT  confirm", SCREEN_H - 14, 1, COL_DIM);
}

void confirmMove(int delta) {
    int old = confirmIndex;
    confirmIndex = (confirmIndex + delta + 2) % 2;
    if (confirmIndex == old) return;
    drawConfirmOption(old, false);
    drawConfirmOption(confirmIndex, true);
}

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

bool homeSlider() {
    Serial.println("Homing...");
    Serial.print("Limit switch reads: ");
    Serial.println(digitalRead(LIMIT_SWITCH_PIN) == HIGH ? "HIGH (not pressed)" : "LOW (pressed)");

    digitalWrite(DIR_PIN, 0);
    long stepsTaken = 0;
    while (digitalRead(LIMIT_SWITCH_PIN) == HIGH) {
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(HOMING_STEP_DELAY_US / 2);
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(HOMING_STEP_DELAY_US / 2);
        stepsTaken++;
        if (stepsTaken >= MAX_HOMING_STEPS) {
            Serial.println("HOMING FAILED: exceeded max travel without hitting the limit switch.");
            Serial.println("Check: motor power connected? DIR polarity correct? Switch wired correctly?");
            return false;
        }
    }
    currentPosition = 0;
    Serial.println("Homed.");
    return true;
}

void moveToColumn(int col) {
    col = constrain(col, 0, NUM_COLUMNS - 1);
    long target = columnSteps[col];
    long diff = target - currentPosition;
    Serial.print("moveToColumn(");
    Serial.print(col);
    Serial.print("): diff=");
    Serial.println(diff);
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

// ------------------------------------------------------------
// Protocol: outgoing
// ------------------------------------------------------------
void send_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t flen = build_frame(cmd, payload, len, frame);
    Serial2.write(frame, flen);
}
void send_ready() { send_frame(CMD_READY, nullptr, 0); }
void send_done(uint8_t col) { uint8_t p[1] = { col }; send_frame(CMD_DONE, p, 1); }
void send_error(uint8_t code) { uint8_t p[1] = { code }; send_frame(CMD_ERROR, p, 1); }
void send_mode_selected(uint8_t mode) {
    uint8_t p[1] = { mode };
    send_frame(CMD_MODE_SELECTED, p, 1);
    Serial.print("SENT: CMD_MODE_SELECTED mode=");
    Serial.println(mode);
}
void send_button(uint8_t btn) {
    uint8_t p[1] = { btn };
    send_frame(CMD_BUTTON, p, 1);
    Serial.print("SENT: CMD_BUTTON btn=");
    Serial.println(btn);
}

void handle_drop_command(uint8_t col, uint8_t color) {
    Serial.print("Received CMD_DROP: col=");
    Serial.print(col);
    Serial.print(" color=");
    Serial.println(color);
    if (col >= NUM_COLUMNS) {
        Serial.println("ERROR: invalid column received");
        send_error(ERR_INVALID_CMD);
        return;
    }
    moveToColumn(col);
    dropPiece(color);

    // Re-home after every drop -- this is purely a MECHANICAL accuracy
    // measure, not how Game Brain learns the board state (that's
    // already tracked reliably in its own software bitboard via
    // CMD_DONE). Step-counting alone has no way to detect a missed
    // step; re-homing after every move means each future column
    // position starts from a freshly-verified real zero instead of
    // letting any drift silently accumulate over a long game.
    if (homeSlider()) {
        send_done(col);
        Serial.println("Sent CMD_DONE");
    } else {
        // The piece DID physically drop successfully -- but position
        // tracking is now unreliable for any future move, so this is
        // treated as a fault rather than silently continuing on a
        // stepper position we can no longer trust.
        send_error(ERR_STEPPER_STALL);
        Serial.println("Post-drop re-home failed -- sent CMD_ERROR instead of CMD_DONE");
    }
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(MOTION_UART_BAUD, SERIAL_8N1, MOTION_UART_RX, MOTION_UART_TX);

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

    digitalWrite(EN_PIN, LOW); // TMC2209 EN is active low

    for (int i = 0; i < NUM_COLUMNS; i++) {
        columnSteps[i] = (long)(i * COLUMN_SPACING_MM * STEPS_PER_MM);
    }

    dispenserServo.attach(SERVO_PIN, 500, 2500);
    dispenserServo.write(SERVO_REST);

    if (TFT_BLK >= 0) {
        pinMode(TFT_BLK, OUTPUT);
        digitalWrite(TFT_BLK, HIGH);
    }
    if (!gfx->begin(SPI_HZ)) {
        Serial.println("DISPLAY INIT FAILED: gfx->begin() returned false.");
        Serial.println("Check TFT_SCK/MOSI/DC/CS/RST wiring and SPI_HZ before trusting anything drawn after this point.");
    } else {
        Serial.println("Display init OK (gfx->begin() returned true).");
    }
    randomSeed(esp_random());

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed -- Roger's face clips will not play.");
        // Menu still works fine without it; clips[].variants just stay 0.
    } else {
        Serial.println("--- clips found ---");
        for (int i = 0; i < CLIP_COUNT; i++) {
            Clip &c = clips[i];
            for (uint8_t v = 0; v < MAX_VARIANTS; v++) {
                uint16_t n = countFrames(c.dir, v);
                if (n == 0) break;
                c.frames[v] = n;
                c.variants = v + 1;
            }
            Serial.print(c.dir);
            Serial.print(": ");
            Serial.print(c.variants);
            Serial.println(" variant(s)");
        }
    }

    drawMenu(); // shown immediately, per design: menu UI is what's on
                 // screen during boot/homing

    if (homeSlider()) {
        send_ready();
        last_ready_beacon = millis();
        Serial.println("Sent CMD_READY");
    } else {
        Serial.println("Homing failed at boot -- NOT sending CMD_READY.");
        Serial.println("Check hardware, then power-cycle this board to retry.");
    }
}

void loop() {
    // ---- UART receive: commands from Game Brain ----
    while (Serial2.available()) {
        if (parser.feed_byte(Serial2.read())) {
            Serial.print("RECV: cmd=0x");
            Serial.print(parser.cmd, HEX);
            Serial.print(" payload[0]=");
            Serial.println(parser.payload[0]);

            if (parser.cmd == CMD_HOME) {
                if (homeSlider()) {
                    send_ready();
                    Serial.println("Sent CMD_READY (re-homed)");
                } else {
                    send_error(ERR_STEPPER_STALL);
                    Serial.println("Re-homing failed -- sent CMD_ERROR");
                }
            } else if (parser.cmd == CMD_DROP) {
                handle_drop_command(parser.payload[0], parser.payload[1]);
            } else if (parser.cmd == CMD_FACE) {
                FaceEvent evt = (FaceEvent)parser.payload[0];
                showSelector = false; // hidden unless CMD_SELECTOR follows
                                        // immediately after, which Game
                                        // Brain always does when pairing
                                        // FACE_IDLE with a human turn
                setClip(face_event_to_clip(evt));
                screenMode = SCREEN_ROGER;
            } else if (parser.cmd == CMD_SELECTOR) {
                selectorCol = parser.payload[0];
                showSelector = true;
                // Physically track the selection live, not just show
                // it on screen -- moveToColumn() is a no-op if the
                // gantry is already there (e.g. this exact column was
                // just dropped in), and blocks briefly (a fraction of
                // a second per column) while it's actually moving,
                // which is the point: the human sees/feels the real
                // drop position while still deciding, not just a
                // screen indicator.
                moveToColumn(selectorCol);
            } else if (parser.cmd == CMD_FAULT) {
                drawFaultScreen(parser.payload[0]);
                screenMode = SCREEN_FAULT;
            }
        }
    }

    // ---- Buttons: meaning depends on which screen owns them ----
    bool pL = debouncedPress(BTN_LEFT_PIN, lastLeft);
    bool pR = debouncedPress(BTN_RIGHT_PIN, lastRight);
    bool pS = debouncedPress(BTN_SELECT_PIN, lastSelect);

    if (screenMode == SCREEN_LOCAL_MENU) {
        if (pL) menuMove(-1);
        if (pR) menuMove(+1);
        if (pS) {
            confirmIndex = 0; // default highlight: "Start Game"
            Serial.print("SELECT pressed on menu -- entering confirm screen for ");
            Serial.println(modeNames[menuIndex]);
            drawConfirm();
            screenMode = SCREEN_LOCAL_CONFIRM;
        }
    } else if (screenMode == SCREEN_LOCAL_CONFIRM) {
        if (pL) confirmMove(-1);
        if (pR) confirmMove(+1);
        if (pS) {
            if (confirmIndex == 0) { // Start Game
                uint8_t mode = (uint8_t)menuIndex; // matches SelectedMode
                                                      // ordering in
                                                      // protocol.h exactly
                Serial.print("SELECT pressed on confirm screen -- starting game, mode=");
                Serial.println(mode);
                send_mode_selected(mode);
                setClip(CLIP_IDLE); // avoid a blank flash while waiting
                                      // for Game Brain's first real
                                      // CMD_FACE
                screenMode = SCREEN_ROGER;
            } else { // Back to Selection
                Serial.println("SELECT pressed on confirm screen -- back to menu");
                drawMenu();
                screenMode = SCREEN_LOCAL_MENU;
            }
        }
    } else if (screenMode == SCREEN_ROGER) {
        if (pL) send_button(0);
        if (pR) send_button(1);
        if (pS) send_button(2);
    }
    // SCREEN_FAULT: buttons intentionally do nothing -- matches Game
    // Brain's own FAULT state, recovery is a physical power cycle

    if (screenMode == SCREEN_ROGER) {
        advanceClipAnimation();
    }

    // ---- CMD_READY beacon: keep announcing readiness while on the
    // local menu, so a missed first CMD_READY doesn't cost 20+ seconds
    // to recover from ----
    if (screenMode == SCREEN_LOCAL_MENU || screenMode == SCREEN_LOCAL_CONFIRM) {
        if (millis() - last_ready_beacon > READY_BEACON_INTERVAL_MS) {
            send_ready();
            last_ready_beacon = millis();
        }
    }
}