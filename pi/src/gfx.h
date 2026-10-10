#pragma once
// ============================================================
// gfx.h -- drawing straight to the Pi's screen, no SDL or X needed
// ============================================================
// Everything is drawn into an 800x480 RGB565 Canvas in memory, then
// Display::present() copies it to /dev/fb0 in one go (no flicker). If the
// framebuffer has a different size or color depth, present() converts and
// scales to fit. --rotate 180 is applied at present time.
#include <stdint.h>
#include <string>
#include <vector>

// Font sizes from Finn's connect4.py (pygame default font). Data lives in
// font_data.h, which only gfx.cpp includes.
enum FontId { F_TITLE, F_ITEM, F_LABEL, F_BIG, F_TABLE, F_SMALL };
struct FontData { int size; int height; const uint8_t* data; const uint16_t* w; const uint32_t* off; };

static inline uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

class Canvas {
public:
    int w, h;
    std::vector<uint16_t> px;
    Canvas(int W = 800, int H = 480) : w(W), h(H), px((size_t)W * H, 0) {}

    void fill(uint16_t c);
    void rect(int x, int y, int rw, int rh, uint16_t c);
    void rectAlpha(int x, int y, int rw, int rh, uint16_t c, int alpha);      // alpha 0..255
    void roundRect(int x, int y, int rw, int rh, int r, uint16_t c);
    void roundRectOutline(int x, int y, int rw, int rh, int r, int t, uint16_t c);
    void circle(int cx, int cy, int r, uint16_t c);
    void hline(int x0, int x1, int y, uint16_t c);
    // Text: y is the TOP of the text box (same as pygame blit).
    int  text(const std::string& s, FontId f, uint16_t c, int x, int y);
    int  textCentered(const std::string& s, FontId f, uint16_t c, int cx, int y);
    int  textRight(const std::string& s, FontId f, uint16_t c, int right, int y);
    static int textWidth(const std::string& s, FontId f);
    static int fontHeight(FontId f);
    // Scale an RGB565 image (sw x sh) to cover this whole canvas (nearest neighbor),
    // optionally rotated 180.
    void blitScaled(const uint16_t* src, int sw, int sh, bool flip180);
    // Draw an RGB565 image into the rectangle (x, y, dw, dh), nearest-neighbor scaled.
    void blit(const uint16_t* src, int sw, int sh, int x, int y, int dw, int dh, bool flip180);

private:
    inline void blend(int x, int y, uint16_t c, int a);
};

class Display {
public:
    // Opens /dev/fb0 and switches the console to graphics mode so the text
    // console can't draw over the game. Returns false if there's no framebuffer.
    bool open(const std::string& dev, std::string& info);
    void present(const Canvas& cv, bool rotate180);
    void close();
    bool isOpen() const { return mem_ != nullptr; }
    ~Display() { close(); }
private:
    int fd_ = -1, tty_ = -1;
    uint8_t* mem_ = nullptr;
    size_t memLen_ = 0;
    int xres_ = 0, yres_ = 0, bpp_ = 0, line_ = 0, yoff_ = 0;
    int roff_ = 11, goff_ = 5, boff_ = 0, rlen_ = 5, glen_ = 6, blen_ = 5;
    std::vector<int> xmap_, ymap_;
    int ox_ = 0, oy_ = 0, dw_ = 0, dh_ = 0;
};

// Restores the text console. Safe to call more than once and from a signal handler.
void gfxRestoreConsole();

// Writes the canvas as a binary PPM (for tests and screenshots).
bool savePPM(const Canvas& cv, const std::string& path);
