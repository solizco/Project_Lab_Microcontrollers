/*
  Connect 4 Robot Face - packed clip player
  -----------------------------------------
  Press the button on GPIO 22 to cycle through the event clips.
  Wire the button between GPIO 22 and GND.

  Reads .mjp files produced by pack.ps1, which bundle all frames of a
  clip into one indexed file. The file is opened once when the clip
  starts and seeked per frame, instead of opening 20 separate files.
  LittleFS file opens were costing about 60 ms each, which was the
  main bottleneck.

  Expected layout after convert.bat + pack.ps1:
    data/idle/0.mjp
    data/idle/1.mjp
    data/good_move/0.mjp
    data/bad_move/0.mjp, 1.mjp
    data/roger_win/0.mjp, 1.mjp
    data/roger_loss/0.mjp, 1.mjp
*/

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <JPEGDEC.h>

// ---------------------------------------------------------------------
// PINS
// ---------------------------------------------------------------------
#define TFT_SCK   18
#define TFT_MOSI  19
#define TFT_DC    13
#define TFT_CS    16
#define TFT_RST   4
#define TFT_BLK   17   // set to -1 if your board has no backlight pin

#define BTN_PIN   22   // cycle button, wired to GND

#define GFX_IPS   true

// SPI clock. Try 60000000 or 80000000 if 40 MHz is stable.
#define SPI_HZ 40000000

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI,
                                            GFX_NOT_DEFINED, VSPI);
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RST, 1 /* landscape */, GFX_IPS, 240, 320);

#define SCREEN_W 320
#define SCREEN_H 240

// Must match SIZE in convert.bat
#define CLIP_W 320
#define CLIP_H 240

#define FRAME_BUF_SIZE 32768
#define MAX_VARIANTS 4
#define MAX_FRAMES   120   // per clip

// Must match FPS in convert.bat: 1000 / fps
const unsigned long frameInterval = 67;   // 15 fps
#define BENCHMARK false

#define TEST_LOOP_ALL true

// ---------------------------------------------------------------------
// EVENTS
// ---------------------------------------------------------------------
enum FaceState {
  FACE_IDLE,
  FACE_GOOD_MOVE,
  FACE_BAD_MOVE,
  FACE_WIN,
  FACE_LOSS,
  FACE_COUNT
};

struct Clip {
  const char *dir;
  bool loopForever;
  uint8_t variants;
  uint16_t frames[MAX_VARIANTS];
};

Clip clips[FACE_COUNT] = {
  {"/idle",       true,  0, {0}},
  {"/good_move",  false, 0, {0}},
  {"/bad_move",   false, 0, {0}},
  {"/roger_win",  true,  0, {0}},
  {"/roger_loss", true,  0, {0}}
};

FaceState currentFace = FACE_IDLE;
uint8_t currentVariant = 0;
uint16_t currentFrame = 0;
unsigned long lastFrameTime = 0;

// The clip file stays open while it plays
File clipFile;
uint32_t frameOffsets[MAX_FRAMES + 1];
uint16_t openFrameCount = 0;

// ---------------------------------------------------------------------
// JPEG DECODING
// ---------------------------------------------------------------------
JPEGDEC jpeg;
static uint8_t frameBuf[FRAME_BUF_SIZE];

const int clipX = (SCREEN_W - CLIP_W) / 2;
const int clipY = (SCREEN_H - CLIP_H) / 2;

bool benchNoDraw = false;

int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (benchNoDraw) return 1;
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels,
                            pDraw->iWidth, pDraw->iHeight);
  return 1;
}

// ---------------------------------------------------------------------
// CLIP FILES
// ---------------------------------------------------------------------
void clipPath(char *buf, size_t n, const char *dir, uint8_t variant) {
  snprintf(buf, n, "%s/%u.mjp", dir, variant);
}

// Reads the header only. Returns frame count, 0 if the file is bad.
uint16_t peekFrameCount(const char *dir, uint8_t variant) {
  char path[64];
  clipPath(path, sizeof(path), dir, variant);
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  char magic[4];
  uint32_t count = 0;
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "MJP1", 4) != 0) {
    Serial.printf("%s: bad header\n", path);
    f.close();
    return 0;
  }
  f.read((uint8_t *)&count, 4);
  f.close();

  if (count == 0 || count > MAX_FRAMES) {
    Serial.printf("%s: %u frames is out of range\n", path, (unsigned)count);
    return 0;
  }
  return (uint16_t)count;
}

// Opens a clip and loads its offset table
bool openClip(const char *dir, uint8_t variant) {
  if (clipFile) clipFile.close();

  char path[64];
  clipPath(path, sizeof(path), dir, variant);
  clipFile = LittleFS.open(path, "r");
  if (!clipFile) {
    Serial.printf("Cannot open %s\n", path);
    return false;
  }

  char magic[4];
  uint32_t count = 0;
  clipFile.read((uint8_t *)magic, 4);
  clipFile.read((uint8_t *)&count, 4);
  if (memcmp(magic, "MJP1", 4) != 0 || count == 0 || count > MAX_FRAMES) {
    Serial.printf("%s: bad file\n", path);
    clipFile.close();
    return false;
  }

  clipFile.read((uint8_t *)frameOffsets, 4 * (count + 1));
  openFrameCount = (uint16_t)count;
  return true;
}

bool drawFrame(uint16_t index) {
  if (!clipFile || index >= openFrameCount) return false;

  unsigned long t0 = micros();

  uint32_t start = frameOffsets[index];
  uint32_t len = frameOffsets[index + 1] - start;
  if (len == 0 || len > FRAME_BUF_SIZE) {
    Serial.printf("Frame %u is %u bytes, out of range\n", index, (unsigned)len);
    return false;
  }

  clipFile.seek(start);
  clipFile.read(frameBuf, len);

  unsigned long tRead = micros() - t0;

  unsigned long t1 = micros();
  if (!jpeg.openRAM(frameBuf, len, jpegDrawCallback)) {
    Serial.printf("Decode failed on frame %u (error %d)\n", index, jpeg.getLastError());
    return false;
  }
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  jpeg.decode(clipX, clipY, 0);
  jpeg.close();
  unsigned long tDrawTotal = micros() - t1;

#if BENCHMARK
  unsigned long t2 = micros();
  benchNoDraw = true;
  jpeg.openRAM(frameBuf, len, jpegDrawCallback);
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  jpeg.decode(clipX, clipY, 0);
  jpeg.close();
  benchNoDraw = false;
  unsigned long tDecode = micros() - t2;

  unsigned long tPush = (tDrawTotal > tDecode) ? (tDrawTotal - tDecode) : 0;

  static uint32_t sRead = 0, sDecode = 0, sPush = 0;
  static uint16_t n = 0;
  sRead += tRead; sDecode += tDecode; sPush += tPush;
  if (++n >= 20) {
    float r = sRead / 20000.0f, d = sDecode / 20000.0f, p = sPush / 20000.0f;
    Serial.printf("read %.1f  decode %.1f  push %.1f ms  ->  real frame %.1f ms (%.1f fps)\n",
                  r, d, p, r + d + p, 1000.0f / (r + d + p));
    sRead = sDecode = sPush = 0; n = 0;
  }
#endif

  return true;
}

// ---------------------------------------------------------------------
// PUBLIC API
// ---------------------------------------------------------------------
void setFace(FaceState f) {
  if (f >= FACE_COUNT) return;
  Clip &c = clips[f];
  if (c.variants == 0) {
    Serial.printf("No clips in %s\n", c.dir);
    return;
  }

  uint8_t v = random(c.variants);
  if (!openClip(c.dir, v)) return;

  currentFace = f;
  currentVariant = v;
  currentFrame = 0;
  lastFrameTime = 0;
  Serial.printf("Playing %s/%u (%u frames)\n", c.dir, v, openFrameCount);
}

void nextFace() {
  for (int step = 1; step <= FACE_COUNT; step++) {
    int next = (currentFace + step) % FACE_COUNT;
    if (clips[next].variants > 0) {
      setFace((FaceState)next);
      return;
    }
  }
}

// ---------------------------------------------------------------------
// BUTTON
// ---------------------------------------------------------------------
bool lastBtnState = HIGH;
unsigned long lastBtnChange = 0;
const unsigned long debounceMs = 40;

bool buttonPressed() {
  bool s = digitalRead(BTN_PIN);
  unsigned long now = millis();
  if (s != lastBtnState && now - lastBtnChange > debounceMs) {
    lastBtnChange = now;
    lastBtnState = s;
    if (s == LOW) return true;
  }
  return false;
}

// ---------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BTN_PIN, INPUT_PULLUP);

  if (TFT_BLK >= 0) {
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);
  }

  gfx->begin(SPI_HZ);
  gfx->fillScreen(BLACK);
  randomSeed(esp_random());

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed. Run Upload Filesystem Image.");
    gfx->setCursor(20, 110);
    gfx->setTextColor(RED);
    gfx->println("No filesystem");
    return;
  }

  Serial.printf("--- clips found (SPI %d Hz) ---\n", SPI_HZ);
  for (int i = 0; i < FACE_COUNT; i++) {
    Clip &c = clips[i];
    for (uint8_t v = 0; v < MAX_VARIANTS; v++) {
      uint16_t n = peekFrameCount(c.dir, v);
      if (n == 0) break;
      c.frames[v] = n;
      c.variants = v + 1;
      Serial.printf("%s/%u.mjp: %u frames\n", c.dir, v, n);
    }
    if (c.variants == 0) Serial.printf("%s: none\n", c.dir);
  }
  Serial.println("Press the button on GPIO 22 to cycle.");

  setFace(FACE_IDLE);
}

void loop() {
  unsigned long now = millis();

  if (buttonPressed()) nextFace();

  if (openFrameCount == 0) return;

  if (now - lastFrameTime >= frameInterval) {
    lastFrameTime = now;
    drawFrame(currentFrame);

    currentFrame++;
    if (currentFrame >= openFrameCount) {
      if (TEST_LOOP_ALL || clips[currentFace].loopForever) {
        currentFrame = 0;
      } else {
        setFace(FACE_IDLE);
      }
    }
  }
}