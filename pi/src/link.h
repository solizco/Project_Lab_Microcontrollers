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

class Link {
public:
    virtual ~Link() {}
    virtual void send(uint8_t cmd, const uint8_t* payload, uint8_t len) = 0;
    // Non-blocking. Returns true and fills `out` if a complete valid frame
    // was waiting. Call repeatedly until it returns false.
    virtual bool poll(Frame& out, uint64_t now_ms) = 0;
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
private:
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

    // observable state for tests
    int  drops_received = 0;
    int  last_drop_col = -1, last_drop_color = -1;
    int  last_selector = -1;
    int  homes_received = 0;

    void pushButton(uint8_t btn);          // 0 left, 1 right, 2 confirm
    void send(uint8_t cmd, const uint8_t* payload, uint8_t len) override;
    bool poll(Frame& out, uint64_t now_ms) override;
private:
    struct Pending { uint64_t due; Frame f; };
    std::vector<Pending> q_;
    uint64_t now_ = 0, next_beacon_ = 0;
    bool started_ = false, heard_pi_ = false;
    void enqueue(uint64_t delay, uint8_t cmd, const uint8_t* p, uint8_t n);
};
