#pragma once
// ============================================================
// motion.h -- the gantry, driven straight from the Pi's GPIO
// ============================================================
// This is the old ESP32 Motion Controller firmware, moved into the Pi
// program. It behaves the same way (homes on the limit switch, moves to a
// column, drops with the servo, RE-HOMES after every drop, forwards the
// buttons and the shutdown button even during long moves) and it still
// speaks the same frames (CMD_READY, CMD_DONE, CMD_BUTTON, CMD_DEBUG ...).
// The only difference is that the "wire" is now a queue in memory:
// MotionLink is a Link, so Brain, screen-only mode and the Link debug
// screen work exactly as they did with the ESP32.
//
// The motion code runs on its own thread, because homing and moves block
// for seconds (busy-waiting between step pulses for even timing) and the
// screen must keep drawing meanwhile.
//
// All hardware access goes through MotionHal, so the tests run the real
// logic against a simulated carriage on a PC.
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "link.h"
#include "pins.h"

#define GANTRY_FW 10      // shown as "Firmware: v10" on Link debug (10+ = Pi GPIO gantry)

class MotionHal {
public:
    enum Line { STEP, DIR, EN, LIMIT, BTN_LEFT, BTN_RIGHT, BTN_SELECT, BTN_SHUTDOWN, N_LINES };
    virtual ~MotionHal() {}
    virtual bool wired(Line l) const = 0;
    virtual void write(Line l, int v) = 0;     // ignored when not wired
    virtual int  read(Line l) = 0;             // 1 (released / open) when not wired
    virtual uint64_t micros() = 0;
    virtual void delayUs(uint32_t us) = 0;     // precise, for step timing
    virtual void sleepMs(uint32_t ms) = 0;     // lets other threads run
    virtual void servoUs(int us) = 0;          // pulse width; 0 = stop the pulses (servo limp)
    virtual bool servoWired() const = 0;
    virtual std::string servoInfo() const = 0; // "hardware PWM on GPIO18", ...
    // Live state, checked against the hardware where possible:
    // "hardware PWM GPIO18: 1644 us, 50 Hz, on (verified)". Default: servoInfo().
    virtual std::string servoReport() { return servoInfo(); }
    // Watch the servo pin itself (not the PWM settings) and report what really comes out:
    // "GPIO18 (pin 12): 50.0 Hz, 1644 us high - OK". fixIt: if hardware PWM is not
    // reaching the pin, switch to software PWM and measure again. "" = can't measure here.
    virtual std::string servoSignalCheck(bool fixIt) { (void)fixIt; return ""; }
};

class MotionController {
public:
    MotionController(MotionHal& hal, const MotionConfig& cfg, const ServoCal& cal, std::string calPath);

    // ESP32 setup(): settle, arm the driver, home (with retries), then report
    // CMD_READY or CMD_ERROR and keep announcing it until Brain says anything.
    // reason: 1 = power-on, 3 = restart (CMD_RESET).
    void boot(uint8_t reason);
    // ESP32 loop(): one pass. Commands from Brain, buttons, automatic homing retries.
    void tick();
    bool restartRequested() const { return restart_; }

    // ---- the in-memory "UART" (thread-safe) ----
    void fromPi(const Frame& f);
    bool toPi(Frame& out);

    // ---- servo calibration from the screen (thread-safe; done on the motion thread) ----
    // slot: 0 rest, 1 red select, 2 red push, 3 black select, 4 black push, 5 wait (ms)
    void calBegin();                 // pauses automatic homing retries while calibrating
    void calSet(int slot, int value);// stores the value; angles also move the servo there (live preview)
    void calTest(int color);         // 0 red, 1 black: a drop right here, gantry does not move
    void calSave();
    void calCheckSignal();           // measure what really leaves the servo pin
    void calEnd();                   // back to rest, automatic retries resume
    struct Status {
        ServoCal cal; int servoNow = -1; bool homed = false; long pos = 0; bool limit = false;
        bool busy = false; int saveResult = 0;   // 0 not saved yet, 1 saved, -1 failed
        std::string servoInfo, signal;           // signal: last "Check signal" result
    };
    Status status();
    void outputsOff() { driverOn(false); servoOff(); }   // motor and servo off (program exit)

    std::function<void(const std::string&)> log;   // may be called from the motion thread
    std::atomic<bool> stop{false};                  // ends long moves early (program exit)

    // test access
    long position() const { return pos_; }
    bool homed() const { return homed_; }
    int  autoHomeTries() const { return autoHomeTries_; }
    const ServoCal& cal() const { return cal_; }
    long columnSteps(int c) const { return colSteps_[c]; }
    bool lateHome() const { return lateHome_; }
    bool idleOff() const { return idleOff_; }
    bool driverEnabled() const { return driverEnabled_; }
    long falseHits() const { return falseHits_; }
    long lastHomeOffset() const { return lastHomeOffset_; }
    long lostStepWarnings() const { return lostStepWarnings_; }
    long chords() const { return chords_; }

private:
    MotionHal& hal_;
    MotionConfig cfg_;
    ServoCal cal_;
    std::string calPath_;
    long colSteps_[7] = {0};
    long pos_ = 0;
    bool homed_ = false;
    int servoNow_ = -1;
    uint8_t bootReason_ = 1;
    bool heardPi_ = false, bootHomeOk_ = false, bootHomePending_ = false;
    uint64_t lastBeacon_ = 0, lastAutoHome_ = 0, lastService_ = 0, lastStatus_ = 0;
    int autoHomeTries_ = 0;
    long noiseRejected_ = 0;
    bool lateHome_ = false;
    bool idleOff_ = false;             // motor switched off after IDLE_MOTOR_OFF_S: home before the next move
    bool driverEnabled_ = false;
    uint64_t lastMotion_ = 0;
    int curDir_ = 0;                   // logical direction of the last setDir (0 = toward home)
    long walk_ = 0;                    // net steps during the current homing (+ away from home)
    long falseHits_ = 0, lostStepWarnings_ = 0, lastHomeOffset_ = 0;
    long expectFrom_ = -1;             // position before this homing (-1 = unknown)
    bool busy_ = false;                // inside a move or homing (heartbeat to Brain)            // a re-home after a drop failed: home before the next move
    uint64_t lastLateTry_ = 0, lastBeat_ = 0, lastReport_ = 0;
    int servoUsNow_ = 0;
    bool homeBeforeMove(const char* why);
    void servoRelax() { if (!cfg_.servoHold && !calActive_) { hal_.servoUs(0); servoUsNow_ = 0; } }
    bool restart_ = false;
    bool autoHoming_ = false, abortHoming_ = false;
    std::atomic<bool> calActive_{false};
    // buttons (0-2) and the shutdown button (3): state must be stable for BUTTON_STABLE_MS
    int stable_[4] = {1, 1, 1, 1}, cand_[4] = {1, 1, 1, 1};
    uint64_t candSince_[4] = {0, 0, 0, 0};
    // left + right chord = select
    int pendingBtn_ = -1;              // an arrow press held back to see if the other arrow follows
    uint64_t pendingAt_ = 0;
    bool chordLatched_ = false;        // a chord fired: ignore both arrows until both are released
    long chords_ = 0;
    void pressEvent(int i);            // stable press of button i (0 left, 1 right, 2 select)
    void releaseEvent(int i);
    void sendBtn(int i) { uint8_t b = (uint8_t)i; send(CMD_BUTTON, &b, 1); }
    uint64_t shutDownSince_ = 0, lastShutSent_ = 0;
    // queues
    std::mutex qm_;
    std::deque<Frame> inbox_, outbox_;
    struct CalCmd { int kind, a, b; };
    std::deque<CalCmd> calq_;
    std::mutex sm_;
    Status st_;
    int saveResult_ = 0;
    std::string signal_;
    uint32_t txFrames_ = 0, rxFrames_ = 0;

    uint64_t ms() { return hal_.micros() / 1000; }
    void say(const std::string& m) { if (log) log(m); }
    void sayf(const char* fmt, ...);
    void send(uint8_t cmd, const uint8_t* p, uint8_t n);
    void sendDebug(uint8_t ev, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0);
    void sendReady() { send(CMD_READY, nullptr, 0); }
    void sendError(uint8_t code) { send(CMD_ERROR, &code, 1); }
    void publish();

    int  enOn() const { return cfg_.enActiveLow ? 0 : 1; }
    void driverOn(bool on) { hal_.write(MotionHal::EN, on ? enOn() : 1 - enOn()); driverEnabled_ = on; }
    void setDir(int towardHome0) { curDir_ = towardHome0; hal_.write(MotionHal::DIR, cfg_.invertDir ? 1 - towardHome0 : towardHome0); }
    bool limitPressed() { return (hal_.read(MotionHal::LIMIT) == 0) != cfg_.limitInvert; }
    bool limitPressedFiltered();
    void serviceInputs();
    void serviceDuringMove() { if (hal_.micros() / 1000 - lastService_ >= 2) { lastService_ = hal_.micros() / 1000; serviceInputs(); } }
    void waitMs(uint32_t ms);
    bool stepOnce(int delayUs);
    bool doSteps(long n, int dir, int delayUs);
    void armDriver();
    bool homeFailed(long steps);
    bool homeSlider();
    bool homeWithRetry();
    void moveToColumn(int col);
    void servoTo(int angle);
    void servoOff() { hal_.servoUs(0); servoNow_ = -1; }
    void dropPiece(uint8_t color);
    void handleFrame(const Frame& f);
    void handleCal(const CalCmd& c);
};

// The Link Brain talks to. Owns the motion thread.
class MotionLink : public Link {
public:
    MotionLink(MotionHal& hal, const MotionConfig& cfg, const ServoCal& cal, std::string calPath);
    ~MotionLink();
    void start();           // boot + loop on a background thread
    void stopThread();      // driver off, servo off, thread joined
    void send(uint8_t cmd, const uint8_t* payload, uint8_t len) override;
    bool poll(Frame& out, uint64_t now_ms) override;
    MotionController mc;
private:
    MotionHal& hal_;
    std::thread th_;
    bool running_ = false;
};
