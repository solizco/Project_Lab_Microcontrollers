#pragma once
// ============================================================
// link.h -- UART link to the ESP32 Motion Controller
// ============================================================
// Two interchangeable implementations behind one interface:
//   SerialLink : real termios port (/dev/serial0, 115200 8N1)
//   SimLink    : fake Motion Controller for laptop testing (--sim)
#include <stdint.h>
#include <string>
#include <vector>
#include "protocol.h"

struct Frame {
    uint8_t cmd = 0;
    uint8_t len = 0;
    uint8_t payload[C4_MAX_PAYLOAD] = {0};
};

// Traffic counters for the Link debug screen.
struct LinkCounters { uint32_t txFrames = 0, rxFrames = 0, rxBytes = 0, badFrames = 0; };

class Link {
public:
    virtual ~Link() {}
    virtual void send(uint8_t cmd, const uint8_t* payload, uint8_t len) = 0;
    // Non-blocking. Returns true and fills `out` if a complete valid frame
    // was waiting. Call repeatedly until it returns false.
    virtual bool poll(Frame& out, uint64_t now_ms) = 0;
    // Debug commands always go to the real Motion Controller, even in screen-only.
    virtual void sendToHardware(uint8_t cmd, const uint8_t* payload, uint8_t len) { send(cmd, payload, len); }
    virtual const LinkCounters& counters() const { return cnt; }
    // Hold the Pi's TX line low for ~300 ms (a serial "break"). With a 1 k
    // resistor from that wire to the ESP32's EN pin (and 10 uF from EN to
    // ground) this resets the Motion Controller, like pressing EN. Without
    // the resistor it does nothing.
    virtual void hardwareBreak() {}
    LinkCounters cnt;
};

// ------------------------------------------------------------
class SerialLink : public Link {
public:
    SerialLink() {}
    ~SerialLink();
    // Opens the port raw, 115200 8N1, no flow control. On failure returns
    // false and error() says why.
    bool open(const std::string& device);
    const std::string& error() const { return err_; }
    void send(uint8_t cmd, const uint8_t* payload, uint8_t len) override;
    bool poll(Frame& out, uint64_t now_ms) override;
    void hardwareBreak() override;
private:
    void endBreak();
    bool breakOn_ = false;
    uint64_t now_ = 0, breakEnd_ = 0;
    int fd_ = -1;
    std::string err_;
    FrameParser parser_;
    uint8_t buf_[256];
    int pos_ = 0, len_ = 0;
};

// ------------------------------------------------------------
// Fake ESP32 for laptop testing. Behaves like the real firmware:
//   - beacons CMD_READY every 2 s until it has received ANY frame from the Pi
//   - CMD_HOME -> CMD_READY after home_ms
//   - CMD_DROP -> CMD_DONE(col) after drop_ms
//   - CMD_SELECTOR is recorded
//   - pushButton() forwards CMD_BUTTON at any time
// Fault injection flags let tests exercise the Pi's error paths.
class SimLink : public Link {
public:
    uint32_t boot_ms = 500, home_ms = 1000, drop_ms = 2000;
    bool fail_home = false;        // answer CMD_HOME with CMD_ERROR(STEPPER_STALL)
    bool drop_no_reply = false;    // swallow CMD_DROP (tests the 15 s timeout)
    bool drop_wrong_col = false;   // reply DONE with the wrong column
    bool drop_error = false;       // reply CMD_ERROR(STEPPER_STALL) instead of DONE
    bool deaf = false;             // never hears the Pi (broken Pi TX wire): keeps beaconing READY
    bool debug_frames = false;     // act like firmware v2: CMD_DEBUG acks, PING replies, RESET
    bool stall_until_reset = false; // cold-start driver fault: homing fails until a RESET (EN press)
    bool not_started = false;      // the ESP32 never came out of reset: sends nothing, hears nothing
    bool break_wired = false;      // the TX-to-EN resistor is fitted: a break resets (starts) the board
    int  breaks_received = 0;
    void pushReady() { enqueue(0, CMD_READY, nullptr, 0); }   // firmware homed by itself and says so
    void hardwareBreak() override;
    void reboot(uint8_t reason = 9);   // restart like a brownout: DBG_BOOT, home, beacon again

    // observable state for tests
    int  drops_received = 0;
    int  last_drop_col = -1, last_drop_color = -1;
    int  last_selector = -1;
    int  homes_received = 0;
    int  resets_received = 0;

    void pushButton(uint8_t btn);          // 0 left, 1 right, 2 confirm
    void pushShutdown() { enqueue(0, CMD_SHUTDOWN, nullptr, 0); }   // shutdown button held
    void send(uint8_t cmd, const uint8_t* payload, uint8_t len) override;
    bool poll(Frame& out, uint64_t now_ms) override;
private:
    struct Pending { uint64_t due; Frame f; };
    std::vector<Pending> q_;
    uint64_t now_ = 0, next_beacon_ = 0;
    bool started_ = false, heard_pi_ = false;
    void enqueue(uint64_t delay, uint8_t cmd, const uint8_t* p, uint8_t n);
};

// ------------------------------------------------------------
// Sits between Brain and the gantry. In screen-only mode every command goes
// to a built-in simulator instead (the gantry never moves), but button
// presses from the ESP32 still come through, so the physical buttons keep
// working. Replies from the real gantry are ignored while screen-only.
class SwitchLink : public Link {
public:
    explicit SwitchLink(Link& gantry) : gantry_(gantry) { screen.home_ms = 150; screen.drop_ms = 400; screen.boot_ms = 100; }
    bool screenOnly = false;
    SimLink screen;                       // the on-screen "gantry"
    void send(uint8_t cmd, const uint8_t* payload, uint8_t len) override {
        if (screenOnly) screen.send(cmd, payload, len); else gantry_.send(cmd, payload, len);
    }
    bool poll(Frame& out, uint64_t now_ms) override {
        Frame f;
        while (gantry_.poll(f, now_ms))   // always drain the real link
            if (!screenOnly || f.cmd == CMD_BUTTON || f.cmd == CMD_SHUTDOWN || f.cmd == CMD_DEBUG) { out = f; return true; }
        return screenOnly && screen.poll(out, now_ms);
    }
    void sendToHardware(uint8_t cmd, const uint8_t* payload, uint8_t len) override { gantry_.send(cmd, payload, len); }
    void hardwareBreak() override { gantry_.hardwareBreak(); }
    const LinkCounters& counters() const override { return gantry_.counters(); }
private:
    Link& gantry_;
};
