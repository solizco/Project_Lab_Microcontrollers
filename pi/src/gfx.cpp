#include "gfx.h"
#include "font_data.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fb.h>
#include <linux/kd.h>
#endif

// ---------------- Canvas ----------------
void Canvas::fill(uint16_t c) { std::fill(px.begin(), px.end(), c); }

void Canvas::hline(int x0, int x1, int y, uint16_t c) {
    if (y < 0 || y >= h) return;
    x0 = std::max(0, x0); x1 = std::min(w - 1, x1);
    for (int x = x0; x <= x1; x++) px[(size_t)y * w + x] = c;
}

void Canvas::rect(int x, int y, int rw, int rh, uint16_t c) {
    for (int yy = std::max(0, y); yy < std::min(h, y + rh); yy++) hline(x, x + rw - 1, yy, c);
}

inline void Canvas::blend(int x, int y, uint16_t c, int a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    uint16_t& d = px[(size_t)y * w + x];
    if (a >= 255) { d = c; return; }
    int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    int sr = c >> 11, sg = (c >> 5) & 63, sb = c & 31;
    dr += ((sr - dr) * a) / 255; dg += ((sg - dg) * a) / 255; db += ((sb - db) * a) / 255;
    d = (uint16_t)((dr << 11) | (dg << 5) | db);
}

void Canvas::rectAlpha(int x, int y, int rw, int rh, uint16_t c, int a) {
    for (int yy = y; yy < y + rh; yy++) for (int xx = x; xx < x + rw; xx++) blend(xx, yy, c, a);
}

void Canvas::roundRect(int x, int y, int rw, int rh, int r, uint16_t c) {
    r = std::min(r, std::min(rw, rh) / 2);
    for (int yy = 0; yy < rh; yy++) {
        int inset = 0;
        int dy = (yy < r) ? r - yy - 1 : (yy >= rh - r ? yy - (rh - r) : -1);
        if (dy >= 0) {   // inside a corner band: shrink the row
            double fx = r - std::sqrt(std::max(0.0, (double)r * r - (dy + 0.5) * (dy + 0.5)));
            inset = (int)(fx + 0.5);
        }
        hline(x + inset, x + rw - 1 - inset, y + yy, c);
    }
}

void Canvas::roundRectOutline(int x, int y, int rw, int rh, int r, int t, uint16_t c) {
    // draw outer then punch the inner with what was there: simplest is ring by rows
    std::vector<uint16_t> saved;
    int ix = x + t, iy = y + t, iw = rw - 2 * t, ih = rh - 2 * t;
    if (iw <= 0 || ih <= 0) { roundRect(x, y, rw, rh, r, c); return; }
    saved.reserve((size_t)iw * ih);
    for (int yy = iy; yy < iy + ih; yy++) for (int xx = ix; xx < ix + iw; xx++)
        saved.push_back((xx >= 0 && yy >= 0 && xx < w && yy < h) ? px[(size_t)yy * w + xx] : 0);
    roundRect(x, y, rw, rh, r, c);
    // restore inner rounded area
    Canvas tmp(iw, ih); tmp.fill(0); tmp.roundRect(0, 0, iw, ih, std::max(0, r - t), 0xFFFF);
    size_t k = 0;
    for (int yy = iy; yy < iy + ih; yy++) for (int xx = ix; xx < ix + iw; xx++, k++)
        if (tmp.px[(size_t)(yy - iy) * iw + (xx - ix)] && xx >= 0 && yy >= 0 && xx < w && yy < h)
            px[(size_t)yy * w + xx] = saved[k];
}

void Canvas::circle(int cx, int cy, int r, uint16_t c) {
    // filled, with a 1-pixel anti-aliased edge
    for (int dy = -r - 1; dy <= r + 1; dy++)
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            double d = std::sqrt((double)dx * dx + (double)dy * dy);
            double a = r + 0.5 - d;
            if (a >= 1) blend(cx + dx, cy + dy, c, 255);
            else if (a > 0) blend(cx + dx, cy + dy, c, (int)(a * 255));
        }
}

void Canvas::ellipse(int cx, int cy, int rx, int ry, uint16_t c) {
    if (rx < 1) rx = 1;
    if (ry < 1) return;
    for (int dy = -ry; dy <= ry; dy++) {
        double fy = (double)dy / ry;
        double half = rx * std::sqrt(std::max(0.0, 1.0 - fy * fy));
        int full = (int)half;
        for (int dx = -full; dx <= full; dx++) blend(cx + dx, cy + dy, c, 255);
        int a = (int)((half - full) * 255);             // anti-aliased left/right edge
        if (a > 0) { blend(cx - full - 1, cy + dy, c, a); blend(cx + full + 1, cy + dy, c, a); }
    }
}

int Canvas::fontHeight(FontId f) { return FONTS[f].height; }

int Canvas::textWidth(const std::string& s, FontId f) {
    int wsum = 0;
    for (unsigned char ch : s) { int i = (ch >= 32 && ch < 127) ? ch - 32 : '?' - 32; wsum += FONTS[f].w[i]; }
    return wsum;
}

int Canvas::text(const std::string& s, FontId f, uint16_t c, int x, int y) {
    const FontData& fd = FONTS[f];
    for (unsigned char ch : s) {
        int i = (ch >= 32 && ch < 127) ? ch - 32 : '?' - 32;
        int gw = fd.w[i];
        const uint8_t* g = fd.data + fd.off[i];
        for (int yy = 0; yy < fd.height; yy++)
            for (int xx = 0; xx < gw; xx++) {
                int a = g[yy * gw + xx];
                if (a) blend(x + xx, y + yy, c, a);
            }
        x += gw;
    }
    return fd.height;
}

int Canvas::textCentered(const std::string& s, FontId f, uint16_t c, int cx, int y) {
    return text(s, f, c, cx - textWidth(s, f) / 2, y);
}
int Canvas::textRight(const std::string& s, FontId f, uint16_t c, int right, int y) {
    return text(s, f, c, right - textWidth(s, f), y);
}

void Canvas::blitScaled(const uint16_t* src, int sw, int sh, bool flip180) {
    if (!src || sw <= 0 || sh <= 0) return;
    if (!flip180 && w == sw * 2 && h == sh * 2) {          // fast path: exact 2x (400x240 -> 800x480)
        for (int y = 0; y < sh; y++) {
            const uint16_t* s = src + (size_t)y * sw;
            uint16_t* d0 = &px[(size_t)(2 * y) * w];
            for (int x = 0; x < sw; x++) { d0[2 * x] = d0[2 * x + 1] = s[x]; }
            memcpy(d0 + w, d0, (size_t)w * 2);
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        int sy = (int)((long)y * sh / h); if (flip180) sy = sh - 1 - sy;
        const uint16_t* s = src + (size_t)sy * sw;
        uint16_t* d = &px[(size_t)y * w];
        for (int x = 0; x < w; x++) {
            int sx = (int)((long)x * sw / w); if (flip180) sx = sw - 1 - sx;
            d[x] = s[sx];
        }
    }
}

void Canvas::blit(const uint16_t* src, int sw, int sh, int x0, int y0, int dw, int dh, bool flip180) {
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int dy = y0 + y; if (dy < 0 || dy >= h) continue;
        int sy = (int)((long)y * sh / dh); if (flip180) sy = sh - 1 - sy;
        const uint16_t* srow = src + (size_t)sy * sw;
        uint16_t* drow = &px[(size_t)dy * w];
        for (int x = 0; x < dw; x++) {
            int dx = x0 + x; if (dx < 0 || dx >= w) continue;
            int sx = (int)((long)x * sw / dw); if (flip180) sx = sw - 1 - sx;
            drow[dx] = srow[sx];
        }
    }
}

// ---------------- Display ----------------
static int g_tty_for_restore = -1;

void gfxRestoreConsole() {
#ifdef __linux__
    if (g_tty_for_restore >= 0) {
        ioctl(g_tty_for_restore, KDSETMODE, KD_TEXT);
        g_tty_for_restore = -1;
    }
#endif
}

bool Display::open(const std::string& dev, std::string& info) {
#ifdef __linux__
    fd_ = ::open(dev.c_str(), O_RDWR);
    if (fd_ < 0) { info = dev + ": " + strerror(errno); return false; }
    fb_var_screeninfo v; fb_fix_screeninfo fx;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &v) || ioctl(fd_, FBIOGET_FSCREENINFO, &fx)) {
        info = "framebuffer info ioctl failed"; ::close(fd_); fd_ = -1; return false;
    }
    xres_ = v.xres; yres_ = v.yres; bpp_ = v.bits_per_pixel; line_ = fx.line_length; yoff_ = v.yoffset;
    roff_ = v.red.offset; goff_ = v.green.offset; boff_ = v.blue.offset;
    rlen_ = v.red.length; glen_ = v.green.length; blen_ = v.blue.length;
    if (bpp_ != 16 && bpp_ != 24 && bpp_ != 32) { info = "unsupported color depth " + std::to_string(bpp_); ::close(fd_); fd_ = -1; return false; }
    memLen_ = fx.smem_len;
    void* m = mmap(nullptr, memLen_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) { info = std::string("mmap: ") + strerror(errno); ::close(fd_); fd_ = -1; return false; }
    mem_ = (uint8_t*)m;

    // Stop the text console from drawing on top of us (cursor, kernel messages).
    tty_ = ::open("/dev/tty", O_RDWR);
    if (tty_ >= 0 && ioctl(tty_, KDSETMODE, KD_GRAPHICS) == 0) g_tty_for_restore = tty_;

    char b[160];
    snprintf(b, sizeof b, "framebuffer %dx%d, %d-bit (R%d@%d G%d@%d B%d@%d)%s", xres_, yres_, bpp_,
             rlen_, roff_, glen_, goff_, blen_, boff_, g_tty_for_restore >= 0 ? "" : ", console NOT in graphics mode");
    info = b;
    return true;
#else
    info = "no framebuffer on this OS"; (void)dev; return false;
#endif
}

void Display::present(const Canvas& cv, bool rotate180) {
    if (!mem_) return;
    // fit the canvas into the screen, keeping its shape, centered
    if (dw_ == 0) {
        double s = std::min((double)xres_ / cv.w, (double)yres_ / cv.h);
        dw_ = (int)(cv.w * s); dh_ = (int)(cv.h * s);
        ox_ = (xres_ - dw_) / 2; oy_ = (yres_ - dh_) / 2;
        xmap_.resize(dw_); ymap_.resize(dh_);
        for (int x = 0; x < dw_; x++) xmap_[x] = std::min(cv.w - 1, (int)((long)x * cv.w / dw_));
        for (int y = 0; y < dh_; y++) ymap_[y] = std::min(cv.h - 1, (int)((long)y * cv.h / dh_));
        // clear the borders once
        for (int y = 0; y < yres_; y++) memset(mem_ + (size_t)(y + yoff_) * line_, 0, (size_t)xres_ * (bpp_ / 8));
    }
    bool direct565 = (bpp_ == 16 && roff_ == 11 && goff_ == 5 && boff_ == 0 && glen_ == 6);
    for (int y = 0; y < dh_; y++) {
        int sy = ymap_[y]; if (rotate180) sy = cv.h - 1 - sy;
        const uint16_t* s = &cv.px[(size_t)sy * cv.w];
        uint8_t* row = mem_ + (size_t)(y + oy_ + yoff_) * line_ + (size_t)ox_ * (bpp_ / 8);
        if (direct565 && dw_ == cv.w && !rotate180) { memcpy(row, s, (size_t)cv.w * 2); continue; }
        for (int x = 0; x < dw_; x++) {
            int sx = xmap_[x]; if (rotate180) sx = cv.w - 1 - sx;
            uint16_t p = s[sx];
            if (direct565) { ((uint16_t*)row)[x] = p; continue; }
            uint32_t r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
            r |= r >> 5; g |= g >> 6; b |= b >> 5;
            uint32_t v = ((r >> (8 - rlen_)) << roff_) | ((g >> (8 - glen_)) << goff_) | ((b >> (8 - blen_)) << boff_);
            if (bpp_ == 32) ((uint32_t*)row)[x] = v;
            else if (bpp_ == 24) { row[3 * x] = v & 255; row[3 * x + 1] = (v >> 8) & 255; row[3 * x + 2] = (v >> 16) & 255; }
            else ((uint16_t*)row)[x] = (uint16_t)v;
        }
    }
}

void Display::close() {
    gfxRestoreConsole();
    if (mem_) { munmap(mem_, memLen_); mem_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (tty_ >= 0) { ::close(tty_); tty_ = -1; }
}

bool savePPM(const Canvas& cv, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", cv.w, cv.h);
    for (uint16_t p : cv.px) {
        unsigned char rgb[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}
