#pragma once
// ============================================================
// pi_hal.h -- MotionHal on a real Raspberry Pi (4, also 5)
// ============================================================
// GPIO: the kernel's GPIO character device (/dev/gpiochipN, uAPI v2).
//   No libraries, no root: the user only needs to be in the "gpio" group.
//   Inputs (limit switch, buttons) get the Pi's internal pull-ups.
// Servo: hardware PWM through /sys/class/pwm when the servo is on GPIO 12,
//   13, 18 or 19 and the pwm overlay is set up (install_autostart.sh does
//   that from pins.conf). Otherwise a software PWM thread (busy-waits the
//   pulse; works on any pin but the servo can twitch now and then).
// Step timing: busy-wait on the monotonic clock for exact pulse spacing.
#include <atomic>
#include <string>
#include <thread>
#include "motion.h"
#include "pins.h"

class PiHal : public MotionHal {
public:
    ~PiHal();
    // Opens the GPIO chip and claims the configured lines. Returns false if
    // the GPIO chip can't be opened or the lines can't be claimed; error()
    // says why. Blank pins are simply skipped.
    bool open(const MotionConfig& cfg);
    const std::string& error() const { return err_; }
    std::string chipInfo() const { return chip_; }

    bool wired(Line l) const override { return idx_[l] >= 0; }
    void write(Line l, int v) override;
    int  read(Line l) override;
    uint64_t micros() override;
    void delayUs(uint32_t us) override;
    void sleepMs(uint32_t ms) override;
    void servoUs(int us) override;
    bool servoWired() const override { return servoMode_ != 0; }
    std::string servoInfo() const override { return servoInfo_; }
    std::string servoReport() override;
    std::string servoSignalCheck(bool fixIt) override;
    // Sample a pin's real level for windowMs (BCM2835/BCM2711 GPLEV register via
    // /dev/gpiomem). edges < 2 = not toggling. Returns false if it can't be read here.
    struct PinScope { int edges = 0, level = -1; double hz = 0, highUs = 0; };
    bool scopePin(int pin, int windowMs, PinScope& out);

    // ---- servo internals, public for --servo-test and the tests ----
    // Sets up only the servo (hardware PWM, else software PWM). open() calls it.
    bool openServo(int pin, bool forceSoftware);
    void setPwmRoot(const std::string& r) { pwmRoot_ = r; }   // tests: a fake /sys/class/pwm
    void setCheckPinctrl(bool on) { checkPinctrl_ = on; }      // tests: no pinctrl tool
    void setGpiomemPath(const std::string& p) { gpiomem_ = p; } // tests: a fake register file
    int servoMode() const { return servoMode_; }               // 0 none, 1 hardware, 2 software
    int servoErrors() const { return pwmErrors_; }

private:
    int reqFd_ = -1;
    int idx_[N_LINES] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int softIdx_ = -1;              // servo line in the request (software PWM)
    int servoMode_ = 0;             // 0 none, 1 hardware PWM, 2 software PWM
    std::string pwmDir_;            // /sys/class/pwm/pwmchipN/pwmM
    std::string pwmChip_;
    int pwmChan_ = 0;
    std::string pwmRoot_ = "/sys/class/pwm";
    bool checkPinctrl_ = true;
    int servoPin_ = -1;
    int lastUs_ = 0;                // last pulse asked for (0 = off)
    int pwmErrors_ = 0;
    std::string chipDev_;           // /dev/gpiochipN, kept for a late switch to software PWM
    int softFd_ = -1;               // line request used by software PWM when it started late
    volatile uint32_t* gpioRegs_ = nullptr;   // /dev/gpiomem, read only (pin levels)
    bool gpioRegsTried_ = false;
    std::string gpiomem_ = "/dev/gpiomem";
    std::string servoInfo_ = "not connected (SERVO is blank)";
    std::string err_, chip_;
    std::atomic<int> softUs_{0};
    std::atomic<bool> softStop_{false};
    std::thread softTh_;
    bool setLine(int idx, int v);
    bool setServoLine(int v);
    bool openHardwarePwm(int pin, std::string& why);
    bool pwmProgram(int us, std::string& why);   // period, polarity, duty, enable + readback
    bool pwmVerify(int us, std::string& why);
    bool startSoftware(const std::string& reason);
    void softLoop();
};
