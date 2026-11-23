// motion.cpp -- the ESP32 Motion Controller firmware (build 3 + console fix),
// ported to run inside the Pi program. Behavior and timings are unchanged;
// comments mark the few places that differ from the ESP32.
#include "motion.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const int HOME_ATTEMPTS = 3;
static const uint32_t BEACON_MS = 2000, AUTO_HOME_EVERY_MS = 3000, DEBOUNCE_MS = 40, SHUTDOWN_HOLD_MS = 2000;
static const int AUTO_HOME_MAX_TRIES = 60;
static const float BACKOFF_MM = 3.0f, MAX_BACKOFF_MM = 20.0f;
static const uint32_t STATUS_LOG_MS = 30000;

MotionController::MotionController(MotionHal& hal, const MotionConfig& cfg, const ServoCal& cal, std::string calPath)
    : hal_(hal), cfg_(cfg), cal_(cal), calPath_(std::move(calPath)) {
    for (int i = 0; i < 7; i++)
        colSteps_[i] = (long)((cfg_.firstColumnMm + i * cfg_.columnSpacingMm) * cfg_.stepsPerMm + 0.5f);
}

void MotionController::sayf(const char* fmt, ...) {
    if (!log) return;
    char b[300]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    log(b);
}

// ---------------- the in-memory UART ----------------
void MotionController::send(uint8_t cmd, const uint8_t* p, uint8_t n) {
    Frame f; f.cmd = cmd; f.len = n > C4_MAX_PAYLOAD ? C4_MAX_PAYLOAD : n;
    if (f.len) memcpy(f.payload, p, f.len);
    { std::lock_guard<std::mutex> g(qm_); outbox_.push_back(f); }
    txFrames_++;
}
void MotionController::sendDebug(uint8_t ev, uint8_t a, uint8_t b, uint8_t c) { uint8_t p[4] = {ev, a, b, c}; send(CMD_DEBUG, p, 4); }
void MotionController::fromPi(const Frame& f) { std::lock_guard<std::mutex> g(qm_); inbox_.push_back(f); }
bool MotionController::toPi(Frame& out) {
    std::lock_guard<std::mutex> g(qm_);
    if (outbox_.empty()) return false;
    out = outbox_.front(); outbox_.pop_front();
    return true;
}

// ---------------- buttons (read everywhere, even mid-move) ----------------
void MotionController::serviceInputs() {
    uint64_t now = ms();
    // A press counts only after the pin has read pressed for BUTTON_STABLE_MS without a
    // break. Motor noise makes short blips on the wires; those never last that long.
    // (ESP32 version: the first low reading counted, then a 40 ms lockout.)
    static const MotionHal::Line L[4] = {MotionHal::BTN_LEFT, MotionHal::BTN_RIGHT, MotionHal::BTN_SELECT, MotionHal::BTN_SHUTDOWN};
    uint64_t stableMs = cfg_.buttonStableMs > 0 ? (uint64_t)cfg_.buttonStableMs : DEBOUNCE_MS / 2;
    for (int i = 0; i < 4; i++) {
        if (!hal_.wired(L[i])) continue;
        int s = hal_.read(L[i]);
        if (s != cand_[i]) { cand_[i] = s; candSince_[i] = now; }
        else if (s != stable_[i] && now - candSince_[i] >= stableMs) {
            stable_[i] = s;
            if (i < 3) { if (s == 0) pressEvent(i); else releaseEvent(i); }
        }
    }
    // an arrow held past the chord window on its own: it was a plain arrow press
    if (pendingBtn_ >= 0 && now - pendingAt_ >= (uint64_t)cfg_.chordWindowMs) { sendBtn(pendingBtn_); pendingBtn_ = -1; }
    // shutdown button: must be held 2 s (a bump does nothing), repeated each second while held
    if (hal_.wired(MotionHal::BTN_SHUTDOWN) && stable_[3] == 0) {
        if (!shutDownSince_) shutDownSince_ = now ? now : 1;
        if (now - shutDownSince_ >= SHUTDOWN_HOLD_MS && (!lastShutSent_ || now - lastShutSent_ >= 1000)) {
            send(CMD_SHUTDOWN, nullptr, 0);
            lastShutSent_ = now ? now : 1;
        }
    } else { shutDownSince_ = 0; lastShutSent_ = 0; }
    // long moves and homing: a sign of life every 2 s (Brain's drop timeout counts silence, not duration)
    if (busy_ && now - lastBeat_ >= 2000) { lastBeat_ = now; sendDebug(DBG_STATUS, (homed_ ? 1 : 0) | (limitPressed() ? 2 : 0), GANTRY_FW, 0); }
    // the calibration screen wants the servo: stop an AUTOMATIC homing attempt (never a commanded one)
    if (autoHoming_ && calActive_) abortHoming_ = true;
}

// Left + right together = select. Two presses never land at the same instant, so
// an arrow press is held back for CHORD_WINDOW_MS: if the other arrow arrives in
// that time it is a select and neither arrow counts. A quick tap is sent the
// moment it is released, so plain arrows don't feel slower. After a chord, both
// arrows are ignored until both are released (no stray move while letting go).
void MotionController::pressEvent(int i) {
    if (i == 2 || !cfg_.chordSelect) { sendBtn(i); return; }        // the select pin itself, or chords off
    if (chordLatched_) return;
    int other = 1 - i;
    bool otherDown = hal_.wired(other == 0 ? MotionHal::BTN_LEFT : MotionHal::BTN_RIGHT) && stable_[other] == 0;
    if (pendingBtn_ == other || otherDown) {
        if (pendingBtn_ != other) say("select (left + right) after a late second press: the first arrow had already counted");
        pendingBtn_ = -1; chordLatched_ = true; chords_++;
        sendBtn(2);
        return;
    }
    pendingBtn_ = i; pendingAt_ = ms();
}
void MotionController::releaseEvent(int i) {
    if (i == 2 || !cfg_.chordSelect) return;
    if (pendingBtn_ == i) { sendBtn(i); pendingBtn_ = -1; }          // quick tap: send it now
    if (chordLatched_ && stable_[0] == 1 && stable_[1] == 1) chordLatched_ = false;
}

void MotionController::waitMs(uint32_t ms_) {        // delay() that keeps the buttons alive
    uint64_t t0 = ms();
    while (ms() - t0 < ms_ && !stop) { serviceInputs(); hal_.sleepMs(1); }
}

// ---------------- mechanics ----------------
bool MotionController::stepOnce(int d) {
    if (stop) return false;
    if (!busy_) { busy_ = true; lastBeat_ = ms(); }
    walk_ += curDir_ ? 1 : -1;
    hal_.write(MotionHal::STEP, 1); hal_.delayUs(d / 2);
    hal_.write(MotionHal::STEP, 0); hal_.delayUs(d / 2);
    serviceDuringMove();
    lastMotion_ = ms();
    return true;
}
bool MotionController::doSteps(long n, int dir, int d) {
    setDir(dir);
    for (long i = 0; i < n; i++) if (!stepOnce(d)) return false;
    return true;
}

// Filtered read for homing. Motor noise couples onto the switch wire while the
// motor steps, so one "pressed" reading proves nothing. Here the motor is paused
// (no step pulses) and the switch must read pressed on every check, 100 us apart,
// for LIMIT_FILTER_MS. A noise spike reads open again within that window and is
// ignored; a real press stays pressed. (ESP32 version: 3 reads, 0.3 ms.)
bool MotionController::limitPressedFiltered() {
    if (!limitPressed()) return false;
    int checks = cfg_.limitFilterMs * 10;
    if (checks < 3) checks = 3;
    for (int i = 0; i < checks; i++) {
        hal_.delayUs(100);
        if (!limitPressed()) { noiseRejected_++; return false; }
    }
    return true;
}

// TMC2209 power-up: an enable seen while its supplies settle may not take. Toggle it.
void MotionController::armDriver() { driverOn(false); waitMs(100); driverOn(true); waitMs(100); }

bool MotionController::homeFailed(long steps) {
    homed_ = false;
    sendDebug(DBG_HOME, 0, (uint8_t)(steps >> 8), (uint8_t)steps);
    return false;
}

bool MotionController::homeSlider() {
    if (!cfg_.motionReady()) { say("homing skipped: STEP, DIR or LIMIT is blank in pins.conf"); return homeFailed(0); }
    uint64_t t0 = ms();
    walk_ = 0;
    bool startPressed = limitPressedFiltered();
    sayf("Homing (limit switch %s)...", startPressed ? "reads PRESSED, backing off to check it" : "open");
    // 1. Already pressed: move AWAY until it releases. Catches a stuck/miswired switch too.
    if (startPressed) {
        long maxBack = (long)(MAX_BACKOFF_MM * cfg_.stepsPerMm), backed = 0;
        setDir(1);
        while (limitPressedFiltered()) {
            if (!stepOnce(cfg_.homingStepDelayUs)) return homeFailed(backed);
            if (autoHoming_ && abortHoming_) { say("homing stopped for servo calibration"); return homeFailed(backed); }
            if (++backed >= maxBack) {
                sayf("HOMING FAILED: limit switch (GPIO%d) still reads PRESSED after backing off %.0f mm. "
                     "The switch is stuck, miswired, or the pin is shorted to ground.", cfg_.limit, MAX_BACKOFF_MM);
                return homeFailed(0);
            }
        }
        if (!doSteps((long)(BACKOFF_MM * cfg_.stepsPerMm), 1, cfg_.homingStepDelayUs)) return homeFailed(backed);
        sayf("Switch released after %ld steps, now approaching it.", backed);
    }
    // 2. Approach the switch until it reads pressed (filtered).
    // 3. Two-touch: back off BACKOFF_MM and touch it again at half speed. A real switch
    //    is there again within a few mm; a noise blip that got through the filter is not,
    //    and the approach just carries on. (Same idea as a 3D printer's homing bump.)
    const long back = (long)(BACKOFF_MM * cfg_.stepsPerMm), maxBack = (long)(MAX_BACKOFF_MM * cfg_.stepsPerMm);
    long steps = 0, falseHitsNow = 0;
    for (;;) {
        setDir(0);
        while (!limitPressedFiltered()) {
            if (!stepOnce(cfg_.homingStepDelayUs)) return homeFailed(steps);
            if (autoHoming_ && abortHoming_) { say("homing stopped for servo calibration"); return homeFailed(steps); }
            if (++steps >= cfg_.maxHomingSteps) {
                if (noiseRejected_) { sayf("  (ignored %ld noise blips on the limit switch wire)", noiseRejected_); noiseRejected_ = 0; }
                sayf("HOMING FAILED: %ld steps without hitting the limit switch (GPIO%d). "
                     "Check the switch wiring, that the motor turned, and INVERT_DIR.", steps, cfg_.limit);
                return homeFailed(steps);
            }
        }
        if (!cfg_.twoTouchHoming) break;
        if (!doSteps(back, 1, cfg_.homingStepDelayUs)) return homeFailed(steps);
        long extra = 0;                                  // switch still pressed 3 mm later: keep backing off
        while (limitPressedFiltered()) {
            if (!stepOnce(cfg_.homingStepDelayUs)) return homeFailed(steps);
            if (++extra >= maxBack) {
                sayf("HOMING FAILED: limit switch (GPIO%d) stays PRESSED while backing off %.0f mm (stuck, or wired to NC: LIMIT_INVERT).",
                     cfg_.limit, MAX_BACKOFF_MM);
                return homeFailed(steps);
            }
        }
        setDir(0);
        long s2 = 0; bool again = false;
        while (s2 <= back * 3 + extra) {
            if (limitPressedFiltered()) { again = true; break; }
            if (!stepOnce(cfg_.homingStepDelayUs * 2)) return homeFailed(steps);
            s2++;
        }
        if (again) break;
        falseHits_++; falseHitsNow++;
        sayf("false limit-switch reading ignored (not there on the second touch, #%ld)", falseHitsNow);
        steps += s2;
        if (falseHitsNow >= 25 || steps >= cfg_.maxHomingSteps) {
            sayf("HOMING FAILED: the limit switch wire is too noisy (%ld false readings). Add a 4.7k pull-up to 3.3 V "
                 "and 0.1 uF to GND on the switch pin, and keep it away from the motor wires.", falseHitsNow);
            return homeFailed(steps);
        }
    }
    // Lost-step check: we knew where we were, so the trip home should match it.
    long travelled = -walk_;                             // net steps toward home during this homing
    if (expectFrom_ > 0) {
        long off = travelled - expectFrom_;
        lastHomeOffset_ = off;
        if (std::labs(off) > (long)(4.0f * cfg_.stepsPerMm)) {
            lostStepWarnings_++;
            sayf("WARNING: home was %.1f mm %s than expected (%s). Re-zeroed, so the next move is right again.",
                 std::labs(off) / cfg_.stepsPerMm, off > 0 ? "farther" : "closer",
                 off > 0 ? "the motor skipped steps or the belt slipped on the way out"
                         : "the switch triggered early: noise, or something touched it");
        }
    }
    pos_ = 0; homed_ = true;
    sayf("Homed after %ld steps (%llu ms).", steps, (unsigned long long)(ms() - t0));
    if (noiseRejected_) { sayf("  (ignored %ld noise blips on the limit switch wire while moving)", noiseRejected_); noiseRejected_ = 0; }
    sendDebug(DBG_HOME, 1, (uint8_t)(steps >> 8), (uint8_t)steps);
    return true;
}

bool MotionController::homeWithRetry() {
    expectFrom_ = homed_ ? pos_ : -1;                    // where we think we are (for the lost-step check)
    if (!driverEnabled_) armDriver();
    for (int a = 1; a <= HOME_ATTEMPTS; a++) {
        if (a > 1) expectFrom_ = -1;
        if (a > 1) { sayf("Homing attempt %d of %d: re-arming the stepper driver first", a, HOME_ATTEMPTS); armDriver(); waitMs(400); }
        if (homeSlider()) return true;
        if (stop || (autoHoming_ && abortHoming_)) return false;
    }
    return false;
}

void MotionController::moveToColumn(int col) {
    if (col < 0) col = 0;
    if (col > 6) col = 6;
    long diff = colSteps_[col] - pos_;
    if (!diff) return;
    uint64_t t0 = ms();
    sayf("Move to column %d: %ld steps %s", col, diff < 0 ? -diff : diff, diff > 0 ? "away from home" : "toward home");
    sendDebug(DBG_MOVE, (uint8_t)col, 0);
    bool ok = doSteps(diff < 0 ? -diff : diff, diff > 0 ? 1 : 0, cfg_.stepDelayUs);
    pos_ = colSteps_[col];
    if (!ok) { homed_ = false; return; }       // stopped early (program exit): position unknown
    sayf("At column %d (%llu ms)", col, (unsigned long long)(ms() - t0));
    sendDebug(DBG_MOVE, (uint8_t)col, 1);
}

void MotionController::servoTo(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    servoUsNow_ = cfg_.servoMinUs + (cfg_.servoMaxUs - cfg_.servoMinUs) * angle / 180;
    hal_.servoUs(servoUsNow_);
    servoNow_ = angle;
}

void MotionController::dropPiece(uint8_t color) {
    int sel = color == 0 ? cal_.redSel : cal_.blackSel, push = color == 0 ? cal_.redPush : cal_.blackPush;
    if (!hal_.servoWired()) say("Servo drop SKIPPED: SERVO is blank in pins.conf or its output failed to start");
    servoTo(sel);       waitMs(cal_.waitMs);
    servoTo(push);      waitMs(cal_.waitMs);
    servoTo(cal_.rest); waitMs(cal_.waitMs);
    sayf("Servo drop done (%s): %d -> %d -> %d deg | servo %s", color == 0 ? "RED" : "BLACK", sel, push, cal_.rest,
         hal_.servoReport().c_str());
    servoRelax();
    sendDebug(DBG_SERVO, color);
}

// Position unknown (a re-home after a drop failed): home first. One more full
// try before anything is given up, so a single bad re-home doesn't end the game.
bool MotionController::homeBeforeMove(const char* why) {
    if (homed_) return true;
    if (!lateHome_ && !idleOff_) return false;
    sayf("%s: homing before %s", idleOff_ ? "motor was resting" : "position unknown after a failed re-home", why);
    if (!homeWithRetry()) return false;
    lateHome_ = false; idleOff_ = false;
    return true;
}

// ---------------- setup() ----------------
void MotionController::boot(uint8_t reason) {
    bootReason_ = reason; restart_ = false;
    homed_ = false; heardPi_ = false; bootHomeOk_ = false; bootHomePending_ = false; autoHomeTries_ = 0;
    abortHoming_ = false;
    sayf("=== gantry on Pi GPIO (v%d) === %s", GANTRY_FW, reason == 3 ? "restart" : "start");
    sayf("pins: STEP %d DIR %d EN %d LIMIT %d SERVO %d | buttons L %d R %d SEL %d SHUTDOWN %d | servo: %s",
         cfg_.step, cfg_.dir, cfg_.en, cfg_.limit, cfg_.servo, cfg_.btnLeft, cfg_.btnRight, cfg_.btnSelect,
         cfg_.btnShutdown, hal_.servoInfo().c_str());
    sayf("column 0 at %.1f mm, spacing %.1f mm, %.2f steps/mm", cfg_.firstColumnMm, cfg_.columnSpacingMm, cfg_.stepsPerMm);
    sendDebug(DBG_BOOT, reason, GANTRY_FW);
    {
        static const char* nm[4] = {"BTN_LEFT", "BTN_RIGHT", "BTN_SELECT", "BTN_SHUTDOWN"};
        uint64_t now = ms();
        for (int i = 0; i < 4; i++) {
            MotionHal::Line l = (MotionHal::Line)(MotionHal::BTN_LEFT + i);
            stable_[i] = cand_[i] = hal_.wired(l) ? hal_.read(l) : 1; candSince_[i] = now;
            if (hal_.wired(l) && stable_[i] == 0)
                sayf("WARNING: %s reads PRESSED at start-up. Stuck button, or its wire touches GND? (ignored until released)", nm[i]);
        }
    }
    idleOff_ = false; lastMotion_ = ms();
    pendingBtn_ = -1; chordLatched_ = false;
    driverOn(false);                       // disabled until things settle
    hal_.write(MotionHal::STEP, 0);
    lateHome_ = false;
    servoTo(cal_.rest);
    if (cfg_.servoBootTest && hal_.servoWired()) {      // visible sign of life, small enough to keep the silos shut
        say("servo start-up test: rest, -8, +8, rest");
        waitMs(400); servoTo(cal_.rest - 8); waitMs(400); servoTo(cal_.rest + 8); waitMs(400); servoTo(cal_.rest);
    }
    waitMs(300);                           // ESP32: 1.5 s for the battery rails; the Pi is up long before this runs
    sayf("servo: %s", hal_.servoReport().c_str());
    if (hal_.servoWired()) {                 // measure the real pin; switches to software PWM if hardware isn't reaching it
        waitMs(60);
        signal_ = hal_.servoSignalCheck(true);
        if (!signal_.empty()) sayf("servo signal check: %s", signal_.c_str());
    }
    servoRelax();
    armDriver();
    autoHoming_ = true; abortHoming_ = false;
    bootHomeOk_ = homeWithRetry();
    autoHoming_ = false;
    bootHomePending_ = !bootHomeOk_;       // tick() keeps retrying every few seconds
    lastAutoHome_ = ms();
    say(bootHomeOk_ ? "Boot homing OK. Announcing READY until the game answers."
                    : "Boot homing FAILED. Announcing ERROR; retrying every 3 s.");
    if (bootHomeOk_) sendReady(); else sendError(ERR_STEPPER_STALL);
    lastBeacon_ = ms();
    publish();
}

void MotionController::handleFrame(const Frame& f) {
    rxFrames_++;
    if (!heardPi_) say("First frame from the game: beacon off.");
    heardPi_ = true;
    sendDebug(DBG_RX, f.cmd, f.len > 0 ? f.payload[0] : 0, f.len > 1 ? f.payload[1] : 0);
    switch (f.cmd) {
    case CMD_HOME:
        if (homeWithRetry()) { bootHomePending_ = false; bootHomeOk_ = true; lateHome_ = false; idleOff_ = false; sendReady(); }
        else sendError(ERR_STEPPER_STALL);
        break;
    case CMD_DROP: {
        if (f.len < 2) { sendError(ERR_INVALID_CMD); break; }
        uint8_t col = f.payload[0], color = f.payload[1];
        if (col >= 7 || color > 1) { say("  drop refused: bad column/color"); sendError(ERR_INVALID_CMD); break; }
        if (!homed_ && (lateHome_ || idleOff_) && !homeBeforeMove("this drop")) { say("  drop refused: still can't home"); sendError(ERR_STEPPER_STALL); break; }
        if (!homed_) { say("  drop refused: not homed"); sendError(ERR_NOT_HOMED); break; }
        moveToColumn(col);
        dropPiece(color);
        // Re-home after EVERY drop: each move starts from a freshly verified zero.
        // Pi version: if that fails, the piece IS in, so report DONE and home again
        // before the next move (ESP32: reported ERROR and ended the game).
        if (!homeWithRetry()) {
            lateHome_ = true; lastLateTry_ = ms();
            say("re-home after the drop FAILED: the piece is in, so reporting DONE; homing again before the next move");
        }
        { uint8_t c = col; send(CMD_DONE, &c, 1); }
        break;
    }
    case CMD_SELECTOR:
        if (!homed_) homeBeforeMove("the selector move");
        if (homed_ && f.len >= 1) moveToColumn(f.payload[0]);
        else if (!homed_) say("  selector ignored: not homed");
        break;
    case CMD_PING:
        sendDebug(DBG_STATUS, (homed_ ? 1 : 0) | (limitPressed() ? 2 : 0), GANTRY_FW, 0);
        break;
    case CMD_RESET:
        say("  restart requested");
        restart_ = true;                   // the thread re-runs boot(3), like the ESP32 rebooting
        break;
    default: break;                        // retired / unknown commands are ignored
    }
}

void MotionController::handleCal(const CalCmd& c) {
    switch (c.kind) {
    case 0: calActive_ = true; break;
    case 1: {
        int* slot[6] = {&cal_.rest, &cal_.redSel, &cal_.redPush, &cal_.blackSel, &cal_.blackPush, &cal_.waitMs};
        if (c.a < 0 || c.a > 5) break;
        int v = c.b;
        if (c.a == 5) { v = v < 50 ? 50 : v > 3000 ? 3000 : v; *slot[5] = v; break; }
        v = v < 0 ? 0 : v > 180 ? 180 : v;
        *slot[c.a] = v; servoTo(v);
        saveResult_ = 0;
        break;
    }
    case 2: say(c.a == 0 ? "calibration: test RED drop" : "calibration: test BLACK drop"); dropPiece((uint8_t)(c.a ? 1 : 0)); servoTo(cal_.rest); break;
    case 3: {
        bool ok = saveServoCal(calPath_, cal_);
        saveResult_ = ok ? 1 : -1;
        sayf("calibration %s: rest %d, red %d/%d, black %d/%d, wait %d ms (%s)", ok ? "saved" : "SAVE FAILED",
             cal_.rest, cal_.redSel, cal_.redPush, cal_.blackSel, cal_.blackPush, cal_.waitMs, calPath_.c_str());
        break;
    }
    case 5: signal_ = hal_.servoSignalCheck(true); sayf("servo signal check: %s", signal_.empty() ? "(cannot measure on this Pi)" : signal_.c_str()); break;
    case 4: calActive_ = false; servoTo(cal_.rest); waitMs(cal_.waitMs); servoRelax(); lastAutoHome_ = ms(); break;
    }
}

// ---------------- loop() ----------------
void MotionController::tick() {
    for (;;) {                             // commands from Brain
        Frame f; bool have = false;
        { std::lock_guard<std::mutex> g(qm_); if (!inbox_.empty()) { f = inbox_.front(); inbox_.pop_front(); have = true; } }
        if (!have) break;
        handleFrame(f);
        if (restart_ || stop) return;
    }
    for (;;) {                             // calibration screen
        CalCmd c{}; bool have = false;
        { std::lock_guard<std::mutex> g(qm_); if (!calq_.empty()) { c = calq_.front(); calq_.pop_front(); have = true; } }
        if (!have) break;
        handleCal(c);
    }
    busy_ = false;
    serviceInputs();
    uint64_t now = ms();
    // idle: motor off to stay cool (and save battery); homes again before the next move
    if (cfg_.idleMotorOffS > 0 && driverEnabled_ && homed_ && !calActive_ && cfg_.motionReady() &&
        now - lastMotion_ >= (uint64_t)cfg_.idleMotorOffS * 1000) {
        driverOn(false); homed_ = false; idleOff_ = true;
        sayf("motor off after %d s idle (homes again before the next move)", cfg_.idleMotorOffS);
    }
    // a re-home after a drop failed: try again in the background while the game waits
    if (lateHome_ && !homed_ && !calActive_ && now - lastLateTry_ >= AUTO_HOME_EVERY_MS) {
        lastLateTry_ = now;
        say("re-trying the home that failed after the last drop");
        if (homeWithRetry()) { lateHome_ = false; say("home recovered"); }
        lastLateTry_ = ms();
    }
    // boot homing failed: keep trying by itself (paused while the servo is being calibrated)
    if (bootHomePending_ && !homed_ && !calActive_ && cfg_.motionReady() && autoHomeTries_ < AUTO_HOME_MAX_TRIES &&
        now - lastAutoHome_ >= AUTO_HOME_EVERY_MS) {
        autoHomeTries_++;
        sayf("Automatic homing retry %d of %d", autoHomeTries_, AUTO_HOME_MAX_TRIES);
        armDriver();
        autoHoming_ = true; abortHoming_ = false;
        bool ok = homeSlider();
        autoHoming_ = false;
        if (ok) { bootHomePending_ = false; bootHomeOk_ = true; say("Homing recovered. Sending READY."); sendReady(); }
        lastAutoHome_ = ms();
    }
    if (homed_) bootHomePending_ = false;
    // announce until Brain has said anything
    if (!heardPi_ && ms() - lastBeacon_ > BEACON_MS) {
        if (bootHomeOk_) sendReady(); else sendError(ERR_STEPPER_STALL);
        lastBeacon_ = ms();
    }
    if (ms() - lastStatus_ >= STATUS_LOG_MS) {
        lastStatus_ = ms();
        sayf("STATUS homed=%d pos=%ld limit=%s motor=%s | false switch hits %ld, lost-step warnings %ld | frames from game %u, to game %u",
             homed_ ? 1 : 0, pos_, limitPressed() ? "PRESSED" : "open", driverEnabled_ ? "on" : "off",
             falseHits_, lostStepWarnings_, rxFrames_, txFrames_);
    }
    publish();
}

// ---------------- calibration API (any thread) ----------------
void MotionController::calBegin()              { std::lock_guard<std::mutex> g(qm_); calActive_ = true; calq_.push_back({0, 0, 0}); }
void MotionController::calSet(int slot, int v) { std::lock_guard<std::mutex> g(qm_); calq_.push_back({1, slot, v}); }
void MotionController::calTest(int color)      { std::lock_guard<std::mutex> g(qm_); calq_.push_back({2, color, 0}); }
void MotionController::calSave()               { std::lock_guard<std::mutex> g(qm_); calq_.push_back({3, 0, 0}); }
void MotionController::calCheckSignal()        { std::lock_guard<std::mutex> g(qm_); calq_.push_back({5, 0, 0}); }
void MotionController::calEnd()                { std::lock_guard<std::mutex> g(qm_); calq_.push_back({4, 0, 0}); }

void MotionController::publish() {
    std::lock_guard<std::mutex> g(sm_);
    st_.signal = signal_;
    st_.cal = cal_; st_.servoNow = servoNow_; st_.homed = homed_; st_.pos = pos_;
    st_.limit = cfg_.limit >= 0 && limitPressed(); st_.saveResult = saveResult_;
    if (st_.servoInfo.empty() || ms() - lastReport_ >= 500) { lastReport_ = ms(); st_.servoInfo = hal_.servoReport(); }
}
MotionController::Status MotionController::status() {
    std::lock_guard<std::mutex> g(sm_);
    Status s = st_;
    { std::lock_guard<std::mutex> q(qm_); s.busy = !calq_.empty(); }
    return s;
}

// ---------------- MotionLink ----------------
MotionLink::MotionLink(MotionHal& hal, const MotionConfig& cfg, const ServoCal& cal, std::string calPath)
    : mc(hal, cfg, cal, std::move(calPath)), hal_(hal) {}
MotionLink::~MotionLink() { stopThread(); }

void MotionLink::start() {
    if (running_) return;
    running_ = true;
    mc.stop = false;
    th_ = std::thread([this] {
        uint8_t reason = 1;
        while (!mc.stop) {
            mc.boot(reason);
            while (!mc.stop && !mc.restartRequested()) { mc.tick(); hal_.sleepMs(1); }
            reason = 3;
        }
        mc.outputsOff();
    });
}
void MotionLink::stopThread() {
    if (!running_) return;
    mc.stop = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}
void MotionLink::send(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    Frame f; f.cmd = cmd; f.len = len > C4_MAX_PAYLOAD ? C4_MAX_PAYLOAD : len;
    if (f.len) memcpy(f.payload, payload, f.len);
    mc.fromPi(f);
    cnt.txFrames++;
}
bool MotionLink::poll(Frame& out, uint64_t) {
    if (!mc.toPi(out)) return false;
    cnt.rxFrames++; cnt.rxBytes += out.len + 5u;
    return true;
}
