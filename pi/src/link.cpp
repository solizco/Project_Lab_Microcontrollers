#include "link.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

// ---------------- SerialLink ----------------
SerialLink::~SerialLink() { if (fd_ >= 0) ::close(fd_); }

bool SerialLink::open(const std::string& device) {
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) { err_ = device + ": " + strerror(errno); return false; }

    termios tio;
    if (tcgetattr(fd_, &tio) != 0) { err_ = std::string("tcgetattr: ") + strerror(errno); return false; }
    cfmakeraw(&tio);                       // no echo, no line editing, no CR/LF mangling
    tio.c_cflag |= (CLOCAL | CREAD);       // ignore modem lines, enable receiver
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= CS8;                    // 8N1, no hardware flow control
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    if (tcsetattr(fd_, TCSANOW, &tio) != 0) { err_ = std::string("tcsetattr: ") + strerror(errno); return false; }
    tcflush(fd_, TCIOFLUSH);               // drop any boot-time garbage
    return true;
}

void SerialLink::send(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if (fd_ < 0) return;
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t n = build_frame(cmd, payload, len, frame);
    if (n == 0) return;
    int off = 0, spins = 0;
    while (off < n && spins < 1000) {
        ssize_t w = ::write(fd_, frame + off, n - off);
        if (w > 0) off += (int)w;
        else if (w < 0 && (errno == EAGAIN || errno == EINTR)) { spins++; usleep(200); }
        else return;                       // real error: give up on this frame
    }
}

bool SerialLink::poll(Frame& out, uint64_t) {
    if (fd_ < 0) return false;
    for (;;) {
        if (pos_ >= len_) {
            ssize_t r = ::read(fd_, buf_, sizeof(buf_));
            if (r <= 0) { pos_ = len_ = 0; return false; }
            pos_ = 0; len_ = (int)r;
        }
        while (pos_ < len_) {
            if (parser_.feed_byte(buf_[pos_++])) {
                out.cmd = parser_.cmd;
                out.len = parser_.payload_len;
                memcpy(out.payload, parser_.payload, sizeof(out.payload));
                return true;               // leftover bytes stay in buf_ for the next call
            }
        }
    }
}

// ---------------- SimLink ----------------
void SimLink::enqueue(uint64_t delay, uint8_t cmd, const uint8_t* p, uint8_t n) {
    Pending e; e.due = now_ + delay; e.f.cmd = cmd; e.f.len = n;
    for (uint8_t i = 0; i < n; i++) e.f.payload[i] = p[i];
    q_.push_back(e);
}

void SimLink::pushButton(uint8_t btn) { enqueue(0, CMD_BUTTON, &btn, 1); }

void SimLink::send(uint8_t cmd, const uint8_t* p, uint8_t n) {
    heard_pi_ = true;                       // stops the READY beacon
    switch (cmd) {
    case CMD_HOME:
        homes_received++;
        if (fail_home) { uint8_t e = ERR_STEPPER_STALL; enqueue(home_ms, CMD_ERROR, &e, 1); }
        else           { enqueue(home_ms, CMD_READY, nullptr, 0); }
        break;
    case CMD_DROP:
        if (n < 2) break;
        drops_received++; last_drop_col = p[0]; last_drop_color = p[1];
        if (drop_no_reply) break;
        if (drop_error)    { uint8_t e = ERR_STEPPER_STALL; enqueue(drop_ms, CMD_ERROR, &e, 1); break; }
        { uint8_t c = drop_wrong_col ? (uint8_t)((p[0] + 1) % 7) : p[0];
          enqueue(drop_ms, CMD_DONE, &c, 1); }
        break;
    case CMD_SELECTOR:
        if (n >= 1) last_selector = p[0];
        break;
    default: break;
    }
}

bool SimLink::poll(Frame& out, uint64_t now_ms) {
    now_ = now_ms;
    if (!started_) { started_ = true; next_beacon_ = now_ + boot_ms; }
    // beacon READY like the real board until the Pi has said anything
    if (!heard_pi_ && now_ >= next_beacon_) {
        enqueue(0, CMD_READY, nullptr, 0);
        next_beacon_ = now_ + 2000;
    }
    // deliver the earliest due frame
    int best = -1;
    for (size_t i = 0; i < q_.size(); i++)
        if (q_[i].due <= now_ && (best < 0 || q_[i].due < q_[best].due)) best = (int)i;
    if (best < 0) return false;
    out = q_[best].f;
    q_.erase(q_.begin() + best);
    return true;
}
