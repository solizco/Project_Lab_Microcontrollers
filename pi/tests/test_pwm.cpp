// The real PiHal servo code against a fake /sys/class/pwm tree.
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/pi_hal.h"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, msg) do { if (!(c)) { g_fail++; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } else g_ok++; } while (0)

static std::string rd(const std::string& p) { std::ifstream f(p); std::string s; std::getline(f, s); return s; }
static void wr(const std::string& p, const std::string& v) { std::ofstream(p) << v; }
static void fakeTree(const std::string& root, const char* polarity = "inversed") {
    if (system(("rm -rf " + root).c_str())) {}
    mkdir(root.c_str(), 0755);
    std::string c = root + "/pwmchip0";
    mkdir(c.c_str(), 0755); wr(c + "/npwm", "2\n"); wr(c + "/export", ""); wr(c + "/unexport", "");
    std::string d = c + "/pwm0";
    mkdir(d.c_str(), 0755);
    wr(d + "/period", "0\n"); wr(d + "/duty_cycle", "0\n"); wr(d + "/enable", "0\n"); wr(d + "/polarity", std::string(polarity) + "\n");
}

int main() {
    const std::string root = "/tmp/c4_fakepwm", d = root + "/pwmchip0/pwm0";
    {
        fakeTree(root);
        PiHal h; h.setPwmRoot(root); h.setCheckPinctrl(false);
        CHECK(h.openServo(18, false) && h.servoMode() == 1, "GPIO18: hardware PWM set up");
        CHECK(rd(d + "/period") == "20000000" && rd(d + "/enable") == "1", "50 Hz period written, channel enabled");
        CHECK(rd(d + "/polarity") == "normal", "an inverted channel is set back to normal polarity");
        h.servoUs(1644);
        CHECK(rd(d + "/duty_cycle") == "1644000", "rest (103 deg) = 1644 us written as 1644000 ns");
        CHECK(h.servoReport().find("verified") != std::string::npos && h.servoErrors() == 0, "readback verified, no errors");
        wr(d + "/enable", "0");                 // something switched the channel off behind our back
        h.servoUs(1167);
        CHECK(rd(d + "/enable") == "1" && rd(d + "/duty_cycle") == "1167000" && h.servoErrors() == 1 && h.servoMode() == 1,
              "channel found off: re-programmed and running again (1 error fixed)");
        h.servoUs(0);
        CHECK(rd(d + "/duty_cycle") == "0" && h.servoReport().find("off (limp)") != std::string::npos, "0 = no pulses, reported limp");
        // the kernel refuses the pulse width: re-program fails too -> software PWM (no GPIO chip here -> FAILED, no crash)
        unlink((d + "/duty_cycle").c_str()); mkdir((d + "/duty_cycle").c_str(), 0755);
        h.servoUs(1500);
        CHECK(h.servoMode() == 0 && h.servoErrors() == 3, "refused writes: tries hardware twice, then gives up on it");
        CHECK(h.servoReport().find("FAILED") != std::string::npos, "...and the report says FAILED with the reason");
        printf("  report: %s\n", h.servoReport().c_str());
    }
    {
        fakeTree(root, "normal");
        PiHal h; h.setPwmRoot(root); h.setCheckPinctrl(false);
        CHECK(!h.openServo(14, false) && h.servoInfo().find("software PWM on GPIO14") != std::string::npos, "GPIO14 has no hardware PWM: software PWM");
        PiHal h2; h2.setPwmRoot(root); h2.setCheckPinctrl(false);
        CHECK(!h2.openServo(18, true) && h2.servoInfo().find("SERVO_FORCE_SOFTWARE") != std::string::npos, "SERVO_FORCE_SOFTWARE = 1 skips hardware PWM");
        PiHal h3; h3.setPwmRoot("/tmp/c4_no_such_pwm"); h3.setCheckPinctrl(false);
        CHECK(!h3.openServo(18, false) && h3.servoInfo().find("overlay") != std::string::npos, "no PWM overlay: says so, software PWM");
        PiHal h4; h4.setPwmRoot(root); h4.setCheckPinctrl(false);
        CHECK(h4.openServo(13, false) == false, "GPIO13 needs channel 1, missing in the fake tree: software PWM");
        PiHal h5;
        CHECK(h5.openServo(-1, false) && !h5.servoWired() && h5.servoInfo().find("blank") != std::string::npos, "SERVO blank: no servo, no crash");
    }
    {   // the pin checker: a pin that never toggles is caught even though the PWM registers look right
        fakeTree(root, "normal");
        { std::ofstream f("/tmp/c4_fake_gpiomem", std::ios::binary); std::string z(4096, '\0'); f << z; }
        PiHal h; h.setPwmRoot(root); h.setCheckPinctrl(false); h.setGpiomemPath("/tmp/c4_fake_gpiomem");
        CHECK(h.openServo(12, false) && h.servoMode() == 1, "GPIO12: hardware PWM (channel 0)");
        h.servoUs(1644);
        CHECK(h.servoReport().find("verified") != std::string::npos, "registers verified...");
        std::string sig = h.servoSignalCheck(true);
        printf("  check: %s\n", sig.c_str());
        CHECK(sig.find("NOT toggling") != std::string::npos && sig.find("pin 32") != std::string::npos,
              "...but the pin itself is dead: the checker says so (GPIO12 = physical pin 32)");
        CHECK(h.servoMode() == 0, "it tried to switch to software PWM (no GPIO chip in this test, so it reports the failure)");
        h.servoUs(0);
        PiHal h2; h2.setPwmRoot(root); h2.setCheckPinctrl(false); h2.setGpiomemPath("/tmp/c4_fake_gpiomem");
        h2.openServo(18, false); h2.servoUs(0);
        CHECK(h2.servoSignalCheck(true).find("limp") != std::string::npos, "servo limp: nothing to measure, says so");
    }
    printf("%d passed, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
