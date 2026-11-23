#include "pi_hal.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <climits>
#include <cstdlib>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t monoUs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}
static bool writeFile(const std::string& path, const std::string& v) {
    int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);     // like the shell's ">", fine on sysfs
    if (fd < 0) return false;
    bool ok = ::write(fd, v.data(), v.size()) == (ssize_t)v.size();
    ::close(fd);
    return ok;
}
static std::string readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char b[256] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
    std::string s(b, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// BCM GPIO number -> physical pin on the 40-pin header (0 = not on the header)
static int physPin(int g) {
    static const int map[28] = {27, 28, 3, 5, 7, 29, 31, 26, 24, 21, 19, 23, 32, 33, 8, 10, 36, 11, 12, 35, 38, 40, 15, 16, 18, 22, 37, 13};
    return g >= 0 && g < 28 ? map[g] : 0;
}

PiHal::~PiHal() {
    if (gpioRegs_) munmap((void*)gpioRegs_, 4096);
    if (softTh_.joinable()) { softStop_ = true; softTh_.join(); }
    if (servoMode_ == 1) { writeFile(pwmDir_ + "/duty_cycle", "0"); writeFile(pwmDir_ + "/enable", "0"); }
    if (softFd_ >= 0) ::close(softFd_);
    if (reqFd_ >= 0) ::close(reqFd_);
}

uint64_t PiHal::micros() { return monoUs(); }
void PiHal::sleepMs(uint32_t ms) { timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L}; nanosleep(&ts, nullptr); }
void PiHal::delayUs(uint32_t us) {
    uint64_t end = monoUs() + us;
    if (us > 3000) {                       // long waits: sleep most of it, spin the end
        uint32_t s = us - 1000;
        timespec ts{(time_t)(s / 1000000), (long)(s % 1000000) * 1000L}; nanosleep(&ts, nullptr);
    }
    while (monoUs() < end) {}
}

bool PiHal::setServoLine(int v) {
    if (softFd_ >= 0) {                                   // claimed on its own (late switch to software PWM)
        gpio_v2_line_values lv{}; lv.mask = 1; lv.bits = v ? 1 : 0;
        return ioctl(softFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &lv) == 0;
    }
    return setLine(softIdx_, v);
}

bool PiHal::setLine(int idx, int v) {
    int fd = reqFd_;
    if (fd < 0 || idx < 0) return false;
    gpio_v2_line_values lv{};
    lv.mask = 1ull << idx;
    lv.bits = v ? (1ull << idx) : 0;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &lv) == 0;
}
void PiHal::write(Line l, int v) { setLine(idx_[l], v); }
int PiHal::read(Line l) {
    if (reqFd_ < 0 || idx_[l] < 0) return 1;
    gpio_v2_line_values lv{};
    lv.mask = 1ull << idx_[l];
    if (ioctl(reqFd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &lv) != 0) return 1;
    return (lv.bits >> idx_[l]) & 1 ? 1 : 0;
}

// The pin must really be switched to its PWM function (the overlay does that).
// `pinctrl get N` (Raspberry Pi OS) says e.g. "18: a5 pd | lo // GPIO18 = PWM0_0".
static int pinIsPwm(int pin) {
    char cmd[64]; snprintf(cmd, sizeof cmd, "pinctrl get %d 2>/dev/null", pin);
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    char b[200] = {0}; size_t n = fread(b, 1, sizeof b - 1, p); pclose(p);
    if (!n) return -1;                     // no pinctrl tool: can't tell
    return strstr(b, "PWM") ? 1 : 0;
}

// Which /sys/class/pwm/pwmchipN drives the header pins: the Pi 4's PWM0 block
// (fe20c000 / 7e20c000) or the Pi 5's RP1 PWM0 (1f00098000). Any other chip
// with enough channels is the last resort.
static std::string findPwmChip(const std::string& root, int ch) {
    ::DIR* d = opendir(root.c_str());   // ::DIR, not MotionHal::DIR
    if (!d) return "";
    std::string best, fallback;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "pwmchip", 7)) continue;
        std::string c = root + "/" + e->d_name;
        if (atoi(readFile(c + "/npwm").c_str()) <= ch) continue;
        char real[PATH_MAX] = {0};
        std::string rp = realpath(c.c_str(), real) ? std::string(real) : c;
        if (rp.find("20c000.pwm") != std::string::npos || rp.find("1f00098000.pwm") != std::string::npos) { best = c; break; }
        if (fallback.empty()) fallback = c;
    }
    closedir(d);
    return best.empty() ? fallback : best;
}

bool PiHal::openHardwarePwm(int pin, std::string& why) {
    int ch = (pin == 12 || pin == 18) ? 0 : (pin == 13 || pin == 19) ? 1 : -1;
    if (ch < 0) { why = "GPIO" + std::to_string(pin) + " has no hardware PWM"; return false; }
    if (checkPinctrl_ && pinIsPwm(pin) == 0) { why = "GPIO" + std::to_string(pin) + " is not set to PWM (run install_autostart.sh after changing SERVO, then reboot)"; return false; }
    if (access(pwmRoot_.c_str(), F_OK) != 0) { why = "no " + pwmRoot_ + " (PWM overlay not loaded: run install_autostart.sh, then reboot)"; return false; }
    std::string chip = findPwmChip(pwmRoot_, ch);
    if (chip.empty()) { why = "no PWM chip with channel " + std::to_string(ch) + " (PWM overlay not loaded)"; return false; }
    std::string dir = chip + "/pwm" + std::to_string(ch);
    if (access(dir.c_str(), F_OK) != 0) writeFile(chip + "/export", std::to_string(ch));
    // udev sets the permissions a moment after the export
    for (int i = 0; i < 100 && access((dir + "/period").c_str(), W_OK) != 0; i++) sleepMs(20);
    if (access((dir + "/period").c_str(), W_OK) != 0) { why = "no permission for " + dir + " (run install_autostart.sh: it adds the PWM rule and the gpio group)"; return false; }
    pwmChip_ = chip; pwmChan_ = ch; pwmDir_ = dir;
    if (!pwmProgram(0, why)) { pwmDir_.clear(); return false; }
    return true;
}

// Full setup of the channel, then read it all back: a write the kernel quietly
// ignored, an inverted channel or a wrong period all show up here.
bool PiHal::pwmProgram(int us, std::string& why) {
    const std::string& d = pwmDir_;
    writeFile(d + "/enable", "0");                       // polarity can only change while off
    writeFile(d + "/duty_cycle", "0");
    if (access((d + "/polarity").c_str(), F_OK) == 0 && readFile(d + "/polarity") != "normal")
        writeFile(d + "/polarity", "normal");
    if (!writeFile(d + "/period", "20000000")) { why = "could not set the 50 Hz period on " + d; return false; }
    if (!writeFile(d + "/duty_cycle", std::to_string(us * 1000))) { why = "could not set the pulse width on " + d; return false; }
    if (!writeFile(d + "/enable", "1")) { why = "could not enable " + d; return false; }
    return pwmVerify(us, why);
}

bool PiHal::pwmVerify(int us, std::string& why) {
    const std::string& d = pwmDir_;
    std::string per = readFile(d + "/period"), duty = readFile(d + "/duty_cycle"), en = readFile(d + "/enable");
    std::string pol = access((d + "/polarity").c_str(), F_OK) == 0 ? readFile(d + "/polarity") : "normal";
    if (per != "20000000") { why = "period reads " + per + " instead of 20000000"; return false; }
    long dv = atol(duty.c_str()), want = (long)us * 1000;
    if (dv < want - 2000 || dv > want + 2000) { why = "pulse width reads " + duty + " ns instead of " + std::to_string(want); return false; }
    if (en != "1") { why = "channel reads enable=" + en + " (not running)"; return false; }
    if (pol != "normal") { why = "polarity reads " + pol + " (inverted output)"; return false; }
    return true;
}

// Software PWM on the same pin, started late (hardware PWM failed while running).
// Claiming the line as a GPIO switches the pin away from its PWM function.
bool PiHal::startSoftware(const std::string& reason) {
    if (servoMode_ == 1) { writeFile(pwmDir_ + "/duty_cycle", "0"); writeFile(pwmDir_ + "/enable", "0"); writeFile(pwmChip_ + "/unexport", std::to_string(pwmChan_)); }
    servoMode_ = 0;
    if (chipDev_.empty() || servoPin_ < 0) { servoInfo_ = "FAILED: hardware PWM broke (" + reason + ") and no GPIO chip for software PWM"; return false; }
    int fd = ::open(chipDev_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { servoInfo_ = "FAILED: hardware PWM broke (" + reason + "), cannot open " + chipDev_; return false; }
    gpio_v2_line_request req{};
    strncpy(req.consumer, "connect4-servo", sizeof req.consumer - 1);
    req.offsets[0] = (uint32_t)servoPin_; req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    int rc = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
    ::close(fd);
    if (rc != 0) { servoInfo_ = "FAILED: hardware PWM broke (" + reason + "), software PWM could not claim GPIO" + std::to_string(servoPin_); return false; }
    softFd_ = req.fd;
    servoMode_ = 2;
    servoInfo_ = "software PWM on GPIO" + std::to_string(servoPin_) + " (switched from hardware: " + reason + ")";
    if (!softTh_.joinable()) { softStop_ = false; softTh_ = std::thread([this] { softLoop(); }); }
    return true;
}

bool PiHal::openServo(int pin, bool forceSoftware) {
    servoPin_ = pin;
    if (pin < 0) return true;
    std::string why = "SERVO_FORCE_SOFTWARE = 1";
    if (!forceSoftware && openHardwarePwm(pin, why)) {
        servoMode_ = 1; servoInfo_ = "hardware PWM on GPIO" + std::to_string(pin);
        return true;
    }
    servoInfo_ = "software PWM on GPIO" + std::to_string(pin) + " (may twitch: " + why + ")";
    return false;                                        // caller claims the pin for software PWM
}

std::string PiHal::servoReport() {
    char b[200];
    if (servoMode_ == 1) {
        std::string why;
        bool ok = pwmVerify(lastUs_, why);
        snprintf(b, sizeof b, "hardware PWM GPIO%d: %d us, %s%s", servoPin_, lastUs_, lastUs_ ? "50 Hz, on" : "off (limp)",
                 ok ? " (verified)" : (" - CHECK FAILED: " + why).c_str());
        return b;
    }
    if (servoMode_ == 2) { snprintf(b, sizeof b, "software PWM GPIO%d: %d us%s", servoPin_, lastUs_, lastUs_ ? ", 50 Hz" : ", off (limp)"); return b; }
    return servoInfo_;
}

bool PiHal::open(const MotionConfig& cfg) {
    // ---- find the Pi's main GPIO chip ----
    int chipFd = -1;
    for (int i = 0; i < 16 && chipFd < 0; i++) {
        std::string dev = "/dev/gpiochip" + std::to_string(i);
        int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) { if (errno == EACCES && err_.empty()) err_ = "no permission for " + dev + " (add the user to the gpio group: run install_autostart.sh)"; continue; }
        gpiochip_info info{};
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
            std::string label = info.label;
            if (label == "pinctrl-bcm2711" || label == "pinctrl-bcm2835" || label == "pinctrl-rp1" || (info.lines >= 28 && label.find("pinctrl") == 0)) {
                chipFd = fd; chip_ = dev + " (" + label + ")"; chipDev_ = dev; break;
            }
        }
        ::close(fd);
    }
    if (chipFd < 0) { if (err_.empty()) err_ = "no Raspberry Pi GPIO chip found in /dev/gpiochip*"; return false; }

    // ---- servo first: hardware PWM if possible (then the pin must NOT be claimed as a GPIO) ----
    bool softServo = cfg.servo >= 0 && !openServo(cfg.servo, cfg.servoForceSoftware);

    // ---- claim every configured line in one request ----
    gpio_v2_line_request req{};
    strncpy(req.consumer, "connect4", sizeof req.consumer - 1);
    int pins[N_LINES] = {cfg.step, cfg.dir, cfg.en, cfg.limit, cfg.btnLeft, cfg.btnRight, cfg.btnSelect, cfg.btnShutdown};
    uint64_t outMask = 0, outVals = 0;
    unsigned n = 0;
    for (int l = 0; l < N_LINES; l++) {
        if (pins[l] < 0) continue;
        idx_[l] = (int)n; req.offsets[n] = (uint32_t)pins[l];
        if (l == STEP || l == DIR || l == EN) {
            outMask |= 1ull << n;
            int start = (l == EN) ? (cfg.enActiveLow ? 1 : 0) : 0;   // motor OFF until the gantry code arms it
            if (start) outVals |= 1ull << n;
        }
        n++;
    }
    if (softServo) { softIdx_ = (int)n; req.offsets[n] = (uint32_t)cfg.servo; outMask |= 1ull << n; n++; }
    if (n == 0) { ::close(chipFd); return true; }      // nothing wired yet: nothing to claim
    req.num_lines = n;
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;   // default: input with pull-up
    req.config.num_attrs = 2;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
    req.config.attrs[0].attr.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.attrs[0].mask = outMask;
    req.config.attrs[1].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[1].attr.values = outVals;
    req.config.attrs[1].mask = outMask;
    if (ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req) != 0) {
        int e = errno;
        err_ = std::string("could not claim the GPIO lines: ") + strerror(e) +
               (e == EBUSY ? " (another program, or a dtoverlay in config.txt, is using one of these pins)" : "");
        ::close(chipFd);
        for (int& i : idx_) i = -1;
        softIdx_ = -1;
        return false;
    }
    ::close(chipFd);
    reqFd_ = req.fd;
    if (softServo) { servoMode_ = 2; softTh_ = std::thread([this] { softLoop(); }); }
    return true;
}

void PiHal::servoUs(int us) {
    if (us < 0) us = 0;
    if (us > 3000) us = 3000;
    lastUs_ = us;
    if (servoMode_ == 1) {
        std::string why;
        if (writeFile(pwmDir_ + "/duty_cycle", std::to_string(us * 1000)) && pwmVerify(us, why)) return;
        if (why.empty()) why = "the pulse width write was refused";
        // 1st fallback: program the whole channel again (period, polarity, enable)
        pwmErrors_++;
        std::string why2;
        if (pwmProgram(us, why2)) return;
        // 2nd fallback: software PWM on the same pin, so drops keep working
        pwmErrors_++;
        if (startSoftware(why2.empty() ? why : why2)) softUs_ = us;
        return;
    }
    if (servoMode_ == 2) softUs_ = us;
}

// 50 Hz software PWM: sleep through most of the 20 ms, busy-wait the edges.
void PiHal::softLoop() {
    uint64_t next = monoUs();
    while (!softStop_) {
        int us = softUs_;
        uint64_t start = next;
        while (monoUs() < start) {}
        if (us > 0) {
            setServoLine(1);
            uint64_t off = start + (uint64_t)us;
            while (monoUs() < off) {}
            setServoLine(0);
        }
        next = start + 20000;
        uint64_t now = monoUs();
        if (next < now) next = now + 1000;                 // fell behind (busy system): restart the cycle
        else if (next - now > 1500) sleepMs((uint32_t)((next - now - 1000) / 1000));
    }
    setServoLine(0);
}

// ---------------- built-in logic analyzer for the servo pin ----------------
bool PiHal::scopePin(int pin, int windowMs, PinScope& out) {
    out = PinScope();
    if (pin < 0 || pin > 31) return false;
    if (!gpioRegsTried_) {
        gpioRegsTried_ = true;
        // Pi 4 / Pi Zero (BCM2711 / BCM2835): /dev/gpiomem is the GPIO block, GPLEV0 at 0x34.
        // (Pi 5's RP1 is laid out differently: not measured there.)
        if (chip_.find("bcm2711") != std::string::npos || chip_.find("bcm2835") != std::string::npos || chip_.empty()) {
            int fd = ::open(gpiomem_.c_str(), O_RDONLY | O_SYNC | O_CLOEXEC);
            if (fd >= 0) {
                void* m = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
                ::close(fd);
                if (m != MAP_FAILED) gpioRegs_ = (volatile uint32_t*)m;
            }
        }
    }
    if (!gpioRegs_) return false;
    const uint32_t bit = 1u << pin;
    uint64_t t0 = monoUs(), end = t0 + (uint64_t)windowMs * 1000;
    int last = (gpioRegs_[13] & bit) ? 1 : 0;
    out.level = last;
    uint64_t lastRise = 0, firstRise = 0; int rises = 0; double highSum = 0; int highs = 0;
    for (uint64_t t = t0; t < end; t = monoUs()) {
        int v = (gpioRegs_[13] & bit) ? 1 : 0;
        if (v == last) continue;
        out.edges++;
        if (v) { if (!firstRise) firstRise = t; lastRise = t; rises++; }
        else if (lastRise) { highSum += (double)(t - lastRise); highs++; }
        last = v;
    }
    if (rises >= 2) out.hz = (rises - 1) * 1e6 / (double)(lastRise - firstRise);
    if (highs) out.highUs = highSum / highs;
    out.level = last;
    return true;
}

std::string PiHal::servoSignalCheck(bool fixIt) {
    if (servoMode_ == 0 || servoPin_ < 0) return servoInfo_;
    char where[48]; snprintf(where, sizeof where, "GPIO%d (pin %d)", servoPin_, physPin(servoPin_));
    if (lastUs_ <= 0) return std::string(where) + ": servo is limp (no pulses asked for), nothing to measure";
    PinScope sc;
    if (!scopePin(servoPin_, 120, sc)) return "";
    auto good = [&](const PinScope& x) { return x.edges >= 8 && x.hz > 40 && x.hz < 60 && x.highUs > lastUs_ - 60 && x.highUs < lastUs_ + 60; };
    char b[260];
    if (good(sc)) {
        snprintf(b, sizeof b, "%s: %.1f Hz, %.0f us high (asked %d us) - the Pi IS sending the right signal. "
                 "If the servo doesn't move: its 5 V, its ground joined to a Pi GND, or the signal wire.", where, sc.hz, sc.highUs, lastUs_);
        return b;
    }
    std::string bad;
    if (sc.edges < 2) bad = std::string("NOT toggling, stuck ") + (sc.level ? "HIGH" : "LOW");
    else { snprintf(b, sizeof b, "wrong signal: %.1f Hz, %.0f us high (asked 50 Hz, %d us)", sc.hz, sc.highUs, lastUs_); bad = b; }
    if (fixIt && servoMode_ == 1) {
        std::string was = std::string("hardware PWM ") + bad;
        if (startSoftware(was)) {
            softUs_ = lastUs_;
            sleepMs(80);
            PinScope s2;
            if (scopePin(servoPin_, 120, s2) && good(s2)) {
                snprintf(b, sizeof b, "%s: hardware PWM was %s, SWITCHED to software PWM: now %.1f Hz, %.0f us - OK", where, bad.c_str(), s2.hz, s2.highUs);
                return b;
            }
            snprintf(b, sizeof b, "%s: hardware PWM was %s; software PWM ALSO fails (%d edges). Pin damaged or shorted? "
                     "Disconnect the servo wire and run --servo-test.", where, bad.c_str(), s2.edges);
            return b;
        }
    }
    snprintf(b, sizeof b, "%s: %s. %s", where, bad.c_str(),
             sc.edges < 2 && sc.level == 0 ? "Something may be pulling it to GND (short, or wrong wire on this pin)."
                                           : "Check SERVO in pins.conf and the wiring.");
    return b;
}
