// Tests for the Pi-GPIO gantry (motion.cpp) and pins.conf, against a simulated
// carriage. Time is simulated: delays advance a clock instead of sleeping.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "../src/brain.h"
#include "../src/motion.h"
#include "../src/pins.h"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, msg) do { if (!(c)) { g_fail++; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } else g_ok++; } while (0)

struct FakeHal : MotionHal {
    MotionConfig cfg;
    uint64_t t = 0;                  // simulated microseconds
    long carriage = 3000;            // steps from the switch (switch pressed at <= 0)
    int pins[N_LINES] = {0, 0, 1, 1, 1, 1, 1, 1};
    bool driverDead = false, enRearms = true, motorInverted = false, limitStuck = false, switchNC = false;
    int noiseEvery = 0; uint64_t noiseUs = 0, noiseUntil = 0;   // a 'noiseUs' burst reading pressed every N steps
    long steps = 0;
    std::vector<int> servo;          // every servoUs() call
    std::function<void(long)> onStep;
    bool wired(Line l) const override {
        int p[N_LINES] = {cfg.step, cfg.dir, cfg.en, cfg.limit, cfg.btnLeft, cfg.btnRight, cfg.btnSelect, cfg.btnShutdown};
        return p[l] >= 0;
    }
    bool enabled() const { return pins[EN] == (cfg.enActiveLow ? 0 : 1) || !wired(EN); }
    void write(Line l, int v) override {
        if (!wired(l)) return;
        if (l == EN && enRearms && pins[EN] != v && v == (cfg.enActiveLow ? 0 : 1)) driverDead = false;   // EN toggle wakes it
        if (l == STEP && v == 1 && pins[STEP] == 0) {
            steps++;
            if (enabled() && !driverDead) { int d = pins[DIR] ^ (motorInverted ? 1 : 0); carriage += d ? 1 : -1; }
            if (noiseEvery && steps % noiseEvery == 0) noiseUntil = t + noiseUs;
            if (onStep) onStep(steps);
        }
        pins[l] = v;
    }
    int read(Line l) override {
        if (!wired(l)) return 1;
        if (l == LIMIT) {
            bool pressed = limitStuck || carriage <= 0;
            int v = pressed ? 0 : 1;
            if (switchNC) v = 1 - v;                 // C + NC wiring: high when pressed
            if (t < noiseUntil) v = 0;               // motor noise pulls the wire low
            return v;
        }
        return pins[l];
    }
    uint64_t micros() override { return t; }
    void delayUs(uint32_t us) override { t += us; }
    void sleepMs(uint32_t ms) override { t += ms * 1000ull; }
    void servoUs(int us) override { servo.push_back(us); }
    bool servoWired() const override { return cfg.servo >= 0; }
    std::string servoInfo() const override { return cfg.servo >= 0 ? "test servo" : "not connected"; }
};

static MotionConfig fullCfg() {
    MotionConfig c; c.step = 23; c.dir = 24; c.en = 7; c.limit = 17; c.servo = 18;
    c.btnLeft = 5; c.btnRight = 6; c.btnSelect = 13; c.btnShutdown = 26;
    return c;
}
static std::vector<Frame> drain(MotionController& mc, std::vector<Frame>* dbg = nullptr) {
    std::vector<Frame> out; Frame f;
    while (mc.toPi(f)) { if (f.cmd == CMD_DEBUG) { if (dbg) dbg->push_back(f); } else out.push_back(f); }
    return out;
}
static Frame fr(uint8_t cmd, std::vector<uint8_t> p = {}) { Frame f; f.cmd = cmd; f.len = (uint8_t)p.size(); for (size_t i = 0; i < p.size(); i++) f.payload[i] = p[i]; return f; }
// hold a button down for `ms`, sampling every 5 ms like the real loop, then let go
static void holdBtn(FakeHal& h, MotionController& mc, MotionHal::Line l, int ms) {
    h.pins[l] = 0; for (int i = 0; i < ms / 5; i++) { h.t += 5000; mc.tick(); }
    h.pins[l] = 1; for (int i = 0; i < 12; i++) { h.t += 5000; mc.tick(); }
}
static int usFor(int deg) { return 500 + 2000 * deg / 180; }

int main() {
    // ================= pins.conf =================
    {
        std::ofstream("/tmp/c4_pins_blank.conf") << PINS_TEMPLATE;
        MotionConfig c;
        CHECK(c.load("/tmp/c4_pins_blank.conf"), "template loads");
        CHECK(!c.motionReady() && c.problems.empty() && c.step == -1 && c.servo == -1, "template: all pins blank, no complaints");
        CHECK(c.missing().size() == 3, "template: STEP, DIR, LIMIT reported missing");
        CHECK(c.stepsPerMm > 39.4f && c.stepsPerMm < 39.5f && c.maxHomingSteps == 11000, "template: mechanics defaults");
        std::ofstream("/tmp/c4_pins_full.conf") <<
            "STEP = 23\nDIR=24  # comment\nEN = GPIO7\nLIMIT = 17\nSERVO = 18\nBTN_LEFT = 5\nBTN_RIGHT = 6\n"
            "BTN_SELECT = 13\nBTN_SHUTDOWN = 23\nINVERT_DIR = 1\nSTEPS_PER_MM = 80\nbogus = 1\nSERVO_MIN_US = 600\n";
        MotionConfig d; d.load("/tmp/c4_pins_full.conf");
        CHECK(d.step == 23 && d.dir == 24 && d.en == 7 && d.limit == 17 && d.servo == 18 && d.motionReady(), "filled pins read, GPIO prefix ok");
        CHECK(d.btnShutdown == -1, "a GPIO used twice is dropped");
        bool dup = false, unk = false; for (auto& p : d.problems) { if (p.find("used for both") != std::string::npos) dup = true; if (p.find("bogus") != std::string::npos || p.find("BOGUS") != std::string::npos) unk = true; }
        CHECK(dup && unk, "duplicate pin and unknown key are reported");
        CHECK(d.invertDir && d.stepsPerMm == 80.0f && d.servoMinUs == 600, "mechanics overrides read");
        std::ofstream("/tmp/c4_pins_bad.conf") << "STEP = 40\nDIR = abc\nLIMIT = 4\n";
        MotionConfig e; e.load("/tmp/c4_pins_bad.conf");
        CHECK(e.step == -1 && e.dir == -1 && e.limit == 4 && e.problems.size() == 2, "out-of-range and non-numeric pins rejected");
        ServoCal sc; sc.rest = 99; sc.waitMs = 300;
        CHECK(saveServoCal("/tmp/c4_test/servo.conf", sc), "servo calibration saves (creates the folder)");
        ServoCal r; CHECK(loadServoCal("/tmp/c4_test/servo.conf", r) && r.rest == 99 && r.waitMs == 300 && r.redSel == 70, "servo calibration loads back");
    }

    // ================= boot and homing =================
    FakeHal hal; hal.cfg = fullCfg();
    ServoCal cal;
    {
        MotionController mc(hal, hal.cfg, cal, "/tmp/c4_test/servo2.conf");
        std::vector<Frame> dbg;
        mc.boot(1);
        auto o = drain(mc, &dbg);
        CHECK(o.size() == 1 && o[0].cmd == CMD_READY, "boot: homes, then READY");
        CHECK(mc.homed() && hal.carriage <= 0 && mc.position() == 0, "carriage at the switch, position 0");
        CHECK(!dbg.empty() && dbg[0].payload[0] == DBG_BOOT && dbg[0].payload[2] == GANTRY_FW, "first frame is DBG_BOOT with the gantry version");
        CHECK(hal.pins[MotionHal::EN] == 0, "driver enabled (EN low) after boot");
        CHECK(!hal.servo.empty() && hal.servo[0] == usFor(cal.rest), "servo starts at rest (103 deg)");

        // beacon until the game speaks
        for (int i = 0; i < 3; i++) { hal.t += 2100000; mc.tick(); }
        o = drain(mc);
        CHECK(o.size() == 3 && o[0].cmd == CMD_READY, "READY repeated every 2 s while the game is silent");
        mc.fromPi(fr(CMD_PING)); mc.tick(); dbg.clear(); o = drain(mc, &dbg);
        bool status = false; for (auto& f : dbg) if (f.payload[0] == DBG_STATUS && (f.payload[1] & 1)) status = true;
        CHECK(status, "PING answered with DBG_STATUS (homed)");
        hal.t += 5000000; mc.tick(); o = drain(mc);
        CHECK(o.empty(), "beacon stops once the game has spoken");

        // drop: move, servo select/push/rest, re-home, DONE
        hal.servo.clear(); long maxCar = 0;
        hal.onStep = [&](long) { if (hal.carriage > maxCar) maxCar = hal.carriage; };
        mc.fromPi(fr(CMD_DROP, {3, 1})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && o[0].payload[0] == 3, "DROP col 3 -> DONE(3)");
        CHECK(maxCar == mc.columnSteps(3), "carriage went exactly to column 3");
        CHECK(hal.carriage <= 0 && mc.homed(), "re-homed after the drop");
        CHECK(hal.servo.size() == 3 && hal.servo[0] == usFor(135) && hal.servo[1] == usFor(110) && hal.servo[2] == usFor(103),
              "black drop: select 135, push 110, rest 103");
        CHECK(mc.columnSteps(0) == (long)(23.6f * 39.46f + 0.5f) && mc.columnSteps(6) == (long)((23.6f + 6 * 33.3f) * 39.46f + 0.5f),
              "column positions: 23.6 mm + n x 33.3 mm");
        hal.onStep = nullptr;

        // selector
        mc.fromPi(fr(CMD_SELECTOR, {5})); mc.tick(); drain(mc);
        CHECK(hal.carriage == mc.columnSteps(5) && mc.position() == mc.columnSteps(5), "SELECTOR moves to the column");
        mc.fromPi(fr(CMD_HOME)); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_READY && hal.carriage <= 0, "HOME -> READY at the switch");

        // buttons work during a move
        long pressAt = hal.steps + 1500;
        hal.onStep = [&](long s) { if (s == pressAt) hal.pins[MotionHal::BTN_LEFT] = 0; if (s == pressAt + 400) hal.pins[MotionHal::BTN_LEFT] = 1; };
        mc.fromPi(fr(CMD_DROP, {6, 0})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 2 && o[0].cmd == CMD_BUTTON && o[0].payload[0] == 0 && o[1].cmd == CMD_DONE, "left button pressed mid-move is sent right away, before DONE");
        hal.onStep = nullptr;
        // select and right while idle
        holdBtn(hal, mc, MotionHal::BTN_SELECT, 80); holdBtn(hal, mc, MotionHal::BTN_RIGHT, 80);
        o = drain(mc);
        CHECK(o.size() == 2 && o[0].payload[0] == 2 && o[1].payload[0] == 1, "select = BUTTON 2, right = BUTTON 1");
        // a bouncing contact counts once
        for (int i = 0; i < 6; i++) { hal.pins[MotionHal::BTN_LEFT] = i % 2; hal.t += 3000; mc.tick(); }
        holdBtn(hal, mc, MotionHal::BTN_LEFT, 80); o = drain(mc);
        CHECK(o.size() == 1, "a bouncing contact, then held = one press");
        // motor noise: 1-10 ms blips on every button wire, many times: no presses at all
        for (int k = 0; k < 40; k++) {
            MotionHal::Line l = (MotionHal::Line)(MotionHal::BTN_LEFT + k % 3);
            hal.pins[l] = 0; for (int i = 0; i <= k % 10; i++) { hal.t += 1000; mc.tick(); }
            hal.pins[l] = 1; hal.t += 7000; mc.tick();
        }
        o = drain(mc);
        CHECK(o.empty(), "40 noise blips (1-10 ms) on the button wires: no phantom presses");
        // shutdown: a bump does nothing, a 2 s hold sends SHUTDOWN
        hal.pins[MotionHal::BTN_SHUTDOWN] = 0; for (int i = 0; i < 10; i++) { hal.t += 50000; mc.tick(); }
        hal.pins[MotionHal::BTN_SHUTDOWN] = 1; mc.tick(); o = drain(mc);
        CHECK(o.empty(), "shutdown bump (0.5 s) ignored");
        hal.pins[MotionHal::BTN_SHUTDOWN] = 0; for (int i = 0; i < 45; i++) { hal.t += 50000; mc.tick(); }
        o = drain(mc);
        CHECK(!o.empty() && o[0].cmd == CMD_SHUTDOWN, "shutdown held 2 s -> SHUTDOWN");
        hal.pins[MotionHal::BTN_SHUTDOWN] = 1; mc.tick(); drain(mc);

        // restart
        mc.fromPi(fr(CMD_RESET)); mc.tick();
        CHECK(mc.restartRequested(), "RESET asks the thread to re-run boot");

        // ---- servo calibration from the screen ----
        hal.servo.clear();
        mc.calBegin(); mc.calSet(0, 95); mc.tick();
        CHECK(mc.cal().rest == 95 && !hal.servo.empty() && hal.servo.back() == usFor(95), "calSet(rest, 95) stores it and moves the servo there");
        mc.calSet(5, 400); mc.calSet(1, 200); mc.tick();
        CHECK(mc.cal().waitMs == 400 && mc.cal().redSel == 180, "wait stored; angles clamp to 0..180");
        hal.servo.clear(); uint64_t t0 = hal.t; mc.calTest(0); mc.tick();
        CHECK(hal.servo.size() >= 3 && hal.servo[0] == usFor(180) && hal.servo[1] == usFor(100) && hal.servo[2] == usFor(95), "test red drop uses the new values");
        CHECK(hal.t - t0 >= 1200000, "waits 400 ms after each servo move");
        CHECK(mc.status().saveResult == 0, "not saved yet");
        mc.calSave(); mc.tick();
        ServoCal back; loadServoCal("/tmp/c4_test/servo2.conf", back);
        CHECK(mc.status().saveResult == 1 && back.rest == 95 && back.waitMs == 400, "Save writes servo.conf");
        mc.calEnd(); mc.tick();
        CHECK(hal.servo.back() == usFor(95), "leaving calibration parks the servo at rest");
    }

    // ================= failures =================
    {
        FakeHal h; h.cfg = fullCfg(); h.carriage = 5000; h.driverDead = true; h.enRearms = false;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1);
        auto o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].payload[0] == ERR_STEPPER_STALL && !mc.homed(), "motor never turns: boot reports ERROR");
        CHECK(h.steps >= 3 * h.cfg.maxHomingSteps, "three homing attempts before giving up");
        mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        for (int i = 0; i < 3; i++) { h.t += 3100000; mc.tick(); }
        CHECK(mc.autoHomeTries() >= 2 && !mc.homed() && drain(mc).empty(), "retries by itself every 3 s, quietly");
        h.driverDead = false;
        h.t += 3100000; mc.tick(); o = drain(mc);
        CHECK(mc.homed() && o.size() == 1 && o[0].cmd == CMD_READY, "driver works again: next retry homes and sends READY");
        mc.fromPi(fr(CMD_DROP, {9, 0})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].payload[0] == ERR_INVALID_CMD, "bad column refused");
    }
    {
        FakeHal h; h.cfg = fullCfg(); h.limitStuck = true;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); auto o = drain(mc);
        CHECK(o[0].cmd == CMD_ERROR && !mc.homed(), "limit switch stuck pressed: homing fails loudly instead of a fake zero");
        mc.fromPi(fr(CMD_DROP, {2, 0})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].payload[0] == ERR_NOT_HOMED, "drop before homing refused (NOT_HOMED)");
    }
    {
        FakeHal h; h.cfg = fullCfg(); h.motorInverted = true; h.cfg.invertDir = true;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); auto o = drain(mc);
        CHECK(o[0].cmd == CMD_READY && h.carriage <= 0, "INVERT_DIR = 1 fixes a motor wired backwards");
        FakeHal h2; h2.cfg = fullCfg(); h2.motorInverted = true;
        MotionController mc2(h2, h2.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc2.boot(1); o = drain(mc2);
        CHECK(o[0].cmd == CMD_ERROR, "...and without it a backwards motor fails homing (not a crash into the end)");
    }
    {
        FakeHal h; h.cfg = fullCfg(); h.cfg.enActiveLow = false;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc);
        CHECK(mc.homed() && h.pins[MotionHal::EN] == 1, "EN_ACTIVE_LOW = 0: EN high while running");
        mc.outputsOff();
        CHECK(h.pins[MotionHal::EN] == 0 && h.servo.back() == 0, "outputsOff: motor off, servo pulses stopped");
    }
    {   // calibration while automatic homing keeps failing: the attempt stops, retries pause
        FakeHal h; h.cfg = fullCfg(); h.carriage = 5000; h.driverDead = true; h.enRearms = false;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc); mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        long s0 = h.steps;
        h.onStep = [&](long s) { if (s == s0 + 300) mc.calBegin(); };
        h.t += 3100000; mc.tick();
        CHECK(h.steps - s0 < 400, "opening calibration stops an automatic homing attempt within a few steps");
        h.onStep = nullptr; long s1 = h.steps;
        for (int i = 0; i < 5; i++) { h.t += 3100000; mc.tick(); }
        CHECK(h.steps == s1, "no automatic retries while calibrating");
        mc.calEnd(); mc.tick(); h.driverDead = false; h.t += 3100000; mc.tick();
        CHECK(mc.homed(), "retries resume after leaving calibration");
    }
    {   // motor noise on the switch wire: 2 ms bursts every 40 steps must not count as home
        FakeHal h; h.cfg = fullCfg(); h.noiseEvery = 40; h.noiseUs = 2000;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); auto o = drain(mc);
        CHECK(o[0].cmd == CMD_READY && h.carriage <= 0 && h.carriage > -5, "noise bursts while stepping are ignored: homes AT the switch");
        mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        long maxCar = 0; h.onStep = [&](long) { if (h.carriage > maxCar) maxCar = h.carriage; };
        mc.fromPi(fr(CMD_DROP, {4, 0})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && maxCar == mc.columnSteps(4) && h.carriage <= 0 && h.carriage > -5,
              "drop + re-home with noise: right column, real home");
        h.onStep = nullptr;
        FakeHal h2; h2.cfg = fullCfg(); h2.noiseEvery = 40; h2.noiseUs = 2000; h2.cfg.limitFilterMs = 0;
        MotionController mc2(h2, h2.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc2.boot(1); drain(mc2);
        CHECK(h2.carriage > 100, "...whereas the old 0.3 ms filter is fooled by the same noise (stops early)");
        FakeHal h3; h3.cfg = fullCfg(); h3.noiseEvery = 1; h3.noiseUs = 5000;   // wire reads pressed almost all the time
        MotionController mc3(h3, h3.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc3.boot(1); drain(mc3);
        CHECK(true, "a constantly noisy wire can't be filtered in software (hardware fix needed)");
    }
    {   // mid-game: the re-home after a drop fails -> DONE anyway, home again before the next move
        FakeHal h; h.cfg = fullCfg();
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc); mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        h.enRearms = false; h.servo.clear();
        size_t servoCalls = 0;
        // the motor stops responding once the servo has dropped the piece (i.e. during the re-home)
        h.onStep = [&](long) { if (h.servo.size() >= 3 && servoCalls == 0) { servoCalls = h.servo.size(); h.driverDead = true; } };
        std::vector<Frame> dbg;
        mc.fromPi(fr(CMD_DROP, {2, 0})); mc.tick(); auto o = drain(mc, &dbg);
        CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && o[0].payload[0] == 2, "re-home fails after the drop: still DONE (the piece is in)");
        CHECK(mc.lateHome() && !mc.homed(), "...and it remembers to home before the next move");
        int beats = 0; for (auto& f : dbg) if (f.payload[0] == DBG_STATUS) beats++;
        CHECK(beats >= 5, "heartbeat (DBG_STATUS) every 2 s during the long failing re-home");
        h.onStep = nullptr; h.driverDead = false;
        h.t += 3100000; mc.tick();
        CHECK(mc.homed() && !mc.lateHome(), "background retry 3 s later homes it again");
        // and if it can't: the NEXT drop tries once more, then reports the error
        h.servo.clear(); servoCalls = 0;
        h.onStep = [&](long) { if (h.servo.size() >= 3 && servoCalls == 0) { servoCalls = h.servo.size(); h.driverDead = true; } };
        mc.fromPi(fr(CMD_DROP, {1, 1})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && mc.lateHome(), "second failed re-home: DONE again, still owed a home");
        h.onStep = nullptr;
        mc.fromPi(fr(CMD_DROP, {4, 0})); mc.tick(); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_ERROR && o[0].payload[0] == ERR_STEPPER_STALL, "next drop: homes first, can't -> ERROR (no blind drop)");
    }
    {   // SERVO_HOLD = 0: limp between drops; SERVO_BOOT_TEST = 1: wiggle at start
        FakeHal h; h.cfg = fullCfg(); h.cfg.servoHold = false; h.cfg.servoBootTest = true;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc);
        bool wiggle = false; for (size_t i = 0; i + 2 < h.servo.size(); i++)
            if (h.servo[i] == usFor(95) && h.servo[i + 1] == usFor(111) && h.servo[i + 2] == usFor(103)) wiggle = true;
        CHECK(wiggle, "boot test: rest -8, +8, back to rest");
        CHECK(h.servo.back() == 0, "SERVO_HOLD = 0: limp after start-up");
        mc.fromPi(fr(CMD_DROP, {0, 1})); mc.tick(); drain(mc);
        CHECK(h.servo.back() == 0 && h.servo.size() >= 4, "SERVO_HOLD = 0: drop, then limp");
        std::ofstream("/tmp/c4_pins_srv.conf") << "SERVO_HOLD = 0\nSERVO_FORCE_SOFTWARE = 1\nSERVO_BOOT_TEST = 1\n";
        MotionConfig c; c.load("/tmp/c4_pins_srv.conf");
        CHECK(!c.servoHold && c.servoForceSoftware && c.servoBootTest && c.problems.empty(), "servo options read from pins.conf");
    }
    {   // Brain: a slow drop that keeps reporting progress is NOT a timeout; a silent one is
        struct ScriptLink : Link {
            std::vector<std::pair<uint64_t, Frame>> q; uint64_t dropAt = 0; bool beat = true, done = false;
            void send(uint8_t cmd, const uint8_t* p, uint8_t n) override {
                Frame f; f.cmd = cmd;
                if (cmd == CMD_HOME) { Frame r; r.cmd = CMD_READY; q.push_back({0, r}); }
                if (cmd == CMD_DROP) { dropAt = 1; Frame d; d.cmd = CMD_DONE; d.len = 1; d.payload[0] = p[0]; pending = d; (void)n; }
            }
            Frame pending; uint64_t lastBeat = 0;
            bool poll(Frame& out, uint64_t now) override {
                if (dropAt == 1) dropAt = now;
                if (dropAt && !done) {
                    if (beat && now - lastBeat >= 2000) { lastBeat = now; Frame b; b.cmd = CMD_DEBUG; b.len = 4; b.payload[0] = DBG_STATUS; out = b; return true; }
                    if (now - dropAt >= 25000) { done = true; out = pending; return true; }
                }
                for (size_t i = 0; i < q.size(); i++) { out = q[i].second; q.erase(q.begin() + i); return true; }
                return false;
            }
        };
        for (int beat = 1; beat >= 0; beat--) {
            ScriptLink l; l.beat = beat; Settings st; Brain b(l, st, "/tmp/c4_test/s.conf", 3);
            uint64_t now = 100; for (int i = 0; i < 50; i++) { now += 20; b.update(now); }
            b.startGame(0, 0); for (int i = 0; i < 10; i++) { now += 20; b.update(now); }
            b.onButton(BTN_CONFIRM);
            for (int i = 0; i < 1500; i++) { now += 20; b.update(now); }
            if (beat) CHECK(b.state == State::HUMAN_TURN && !b.board.empty(), "25 s drop with heartbeats: completes, no timeout fault");
            else CHECK(b.state == State::FAULT && b.faultCode == FAULT_TIMEOUT, "25 s of silence: timeout fault as before");
        }
    }
    {   // noise strong enough to pass the 3 ms filter: two-touch homing still finds the real switch
        FakeHal h; h.cfg = fullCfg(); h.carriage = 6000; h.noiseEvery = 700; h.noiseUs = 6000;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); auto o = drain(mc);
        CHECK(o[0].cmd == CMD_READY && h.carriage <= 0 && h.carriage > -5, "6 ms noise bursts: second touch exposes them, homes AT the switch");
        CHECK(mc.falseHits() >= 3, "...false hits were seen and ignored");
        printf("  false hits ignored: %ld\n", mc.falseHits());
        FakeHal h2; h2.cfg = fullCfg(); h2.carriage = 6000; h2.noiseEvery = 700; h2.noiseUs = 6000; h2.cfg.twoTouchHoming = false;
        MotionController mc2(h2, h2.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc2.boot(1); drain(mc2);
        CHECK(h2.carriage > 100, "...with TWO_TOUCH_HOMING = 0 the same noise fakes a home");
    }
    {   // lost steps: the motor slips 300 steps on the way out -> the re-home notices
        FakeHal h; h.cfg = fullCfg();
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc); mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        long skipped = 0; h.onStep = [&](long) { if (h.pins[MotionHal::DIR] == 1 && skipped < 300) { h.carriage--; skipped++; } };
        mc.fromPi(fr(CMD_DROP, {5, 0})); mc.tick(); auto o = drain(mc);
        h.onStep = nullptr;
        CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && mc.lostStepWarnings() == 1 && mc.lastHomeOffset() < -250,
              "300 missed steps: DONE, re-zeroed, and a lost-step warning logged");
        mc.fromPi(fr(CMD_DROP, {2, 1})); mc.tick(); drain(mc);
        CHECK(mc.lostStepWarnings() == 1, "a clean drop gives no warning");
    }
    {   // idle: motor off after IDLE_MOTOR_OFF_S, homes before the next move
        FakeHal h; h.cfg = fullCfg(); h.cfg.idleMotorOffS = 60;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc); mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        for (int i = 0; i < 59; i++) { h.t += 1000000; mc.tick(); }
        CHECK(mc.driverEnabled(), "still on after 59 s");
        h.t += 2000000; mc.tick();
        CHECK(!mc.driverEnabled() && mc.idleOff() && h.pins[MotionHal::EN] == 1 && !mc.homed(), "60 s idle: motor off, marked as needing a home");
        h.carriage += 40;                      // someone nudged the gantry while it was limp
        long maxCar = 0; h.onStep = [&](long) { if (h.carriage > maxCar) maxCar = h.carriage; };
        mc.fromPi(fr(CMD_DROP, {3, 0})); mc.tick(); auto o = drain(mc);
        h.onStep = nullptr;
        CHECK(o.size() == 1 && o[0].cmd == CMD_DONE && maxCar == mc.columnSteps(3) && mc.driverEnabled(),
              "next drop: motor on, homes first, lands on the right column");
        FakeHal h2; h2.cfg = fullCfg(); h2.cfg.idleMotorOffS = 0;
        MotionController mc2(h2, h2.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc2.boot(1); drain(mc2); for (int i = 0; i < 100; i++) { h2.t += 10000000; mc2.tick(); }
        CHECK(mc2.driverEnabled(), "IDLE_MOTOR_OFF_S = 0: never turns off");
    }
    {   // a button stuck down at start-up: warned, no press until it is released and pressed again
        FakeHal h; h.cfg = fullCfg(); h.pins[MotionHal::BTN_SELECT] = 0;
        std::string logText; MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.log = [&](const std::string& m) { logText += m + "\n"; };
        mc.boot(1); drain(mc);
        for (int i = 0; i < 20; i++) { h.t += 10000; mc.tick(); }
        auto o = drain(mc);
        CHECK(o.empty() && logText.find("BTN_SELECT reads PRESSED at start-up") != std::string::npos, "stuck button: warning, no phantom press");
        h.pins[MotionHal::BTN_SELECT] = 1; for (int i = 0; i < 10; i++) { h.t += 10000; mc.tick(); }
        holdBtn(h, mc, MotionHal::BTN_SELECT, 80); o = drain(mc);
        CHECK(o.size() == 1 && o[0].payload[0] == 2, "after release it works normally");
        std::ofstream("/tmp/c4_pins_new.conf") << "TWO_TOUCH_HOMING = 0\nBUTTON_STABLE_MS = 40\nIDLE_MOTOR_OFF_S = 10\n";
        MotionConfig c; c.load("/tmp/c4_pins_new.conf");
        CHECK(!c.twoTouchHoming && c.buttonStableMs == 40 && c.idleMotorOffS == 30 && c.problems.size() == 1, "new settings read; idle off raised to 30 s minimum");
    }
    {   // left + right together = select
        FakeHal h; h.cfg = fullCfg(); h.cfg.btnSelect = -1;          // no select button wired
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc); mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        auto run = [&](int ms) { for (int i = 0; i < ms / 5; i++) { h.t += 5000; mc.tick(); } };
        auto L = MotionHal::BTN_LEFT, R = MotionHal::BTN_RIGHT;
        auto btns = [&]() { std::vector<int> v; Frame f; while (mc.toPi(f)) if (f.cmd == CMD_BUTTON) v.push_back(f.payload[0]); return v; };
        // both within 40 ms, held, released one after the other
        h.pins[L] = 0; run(40); h.pins[R] = 0; run(250); h.pins[L] = 1; run(60); h.pins[R] = 1; run(100);
        auto v = btns();
        CHECK(v.size() == 1 && v[0] == 2, "left then right 40 ms apart = ONE select, no arrows");
        // right first this time, almost simultaneous
        h.pins[R] = 0; run(5); h.pins[L] = 0; run(200); h.pins[R] = 1; h.pins[L] = 1; run(100);
        v = btns(); CHECK(v.size() == 1 && v[0] == 2, "right then left 5 ms apart = select");
        // plain taps still work and are sent on release (no added delay)
        h.pins[L] = 0; run(80); h.pins[L] = 1; run(30); v = btns();
        CHECK(v.size() == 1 && v[0] == 0, "quick left tap = LEFT, sent on release");
        h.pins[R] = 0; run(400); v = btns();
        CHECK(v.size() == 1 && v[0] == 1, "right held 400 ms = one RIGHT (after the chord window)");
        h.pins[R] = 1; run(60); CHECK(btns().empty(), "releasing it adds nothing");
        // after a select, letting go and re-pressing one arrow before both are up = nothing
        h.pins[L] = 0; h.pins[R] = 0; run(200); h.pins[L] = 1; run(60); h.pins[L] = 0; run(200); h.pins[L] = 1; run(40);
        v = btns(); CHECK(v.size() == 1 && v[0] == 2, "fumbled release after a select: no stray arrows");
        h.pins[R] = 1; run(60);
        h.pins[L] = 0; run(80); h.pins[L] = 1; run(30); v = btns();
        CHECK(v.size() == 1 && v[0] == 0, "once both are up, arrows work again");
        // motor noise on the right wire while left is pressed: still just LEFT
        h.pins[L] = 0;
        for (int k = 0; k < 6; k++) { h.pins[R] = 0; run(10); h.pins[R] = 1; run(10); }
        h.pins[L] = 1; run(40); v = btns();
        CHECK(v.size() == 1 && v[0] == 0, "noise blips on the other arrow don't make a select");
        // slow second press (left held 300 ms first): LEFT, then select
        h.pins[L] = 0; run(300); h.pins[R] = 0; run(100); h.pins[L] = 1; h.pins[R] = 1; run(60); v = btns();
        CHECK(v.size() == 2 && v[0] == 0 && v[1] == 2, "late second press: the first arrow already counted, then select");
        CHECK(mc.chords() == 4, "4 selects counted");
        // during a long move the chord still works
        mc.fromPi(fr(CMD_HOME)); mc.tick(); btns();
        long at = h.steps + 2000;
        h.onStep = [&](long s2) { if (s2 == at) { h.pins[L] = 0; h.pins[R] = 0; } if (s2 == at + 600) { h.pins[L] = 1; h.pins[R] = 1; } };
        mc.fromPi(fr(CMD_SELECTOR, {6})); mc.tick(); h.onStep = nullptr; run(50);
        v = btns(); CHECK(v.size() == 1 && v[0] == 2, "select chord pressed during a gantry move");
    }
    {   // CHORD_SELECT = 0: both arrows are just two arrows; the select pin still works
        FakeHal h; h.cfg = fullCfg(); h.cfg.chordSelect = false;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); drain(mc); mc.fromPi(fr(CMD_PING)); mc.tick(); drain(mc);
        h.pins[MotionHal::BTN_LEFT] = 0; h.pins[MotionHal::BTN_RIGHT] = 0;
        for (int i = 0; i < 20; i++) { h.t += 5000; mc.tick(); }
        auto o = drain(mc);
        CHECK(o.size() == 2 && o[0].payload[0] == 0 && o[1].payload[0] == 1, "chords off: LEFT and RIGHT");
        h.pins[MotionHal::BTN_LEFT] = 1; h.pins[MotionHal::BTN_RIGHT] = 1;
        for (int i = 0; i < 20; i++) { h.t += 5000; mc.tick(); }
        holdBtn(h, mc, MotionHal::BTN_SELECT, 80); o = drain(mc);
        CHECK(o.size() == 1 && o[0].payload[0] == 2, "the select pin still works");
        std::ofstream("/tmp/c4_pins_chord.conf") << "CHORD_SELECT = 0\nCHORD_WINDOW_MS = 200\n";
        MotionConfig c; c.load("/tmp/c4_pins_chord.conf");
        CHECK(!c.chordSelect && c.chordWindowMs == 200 && c.problems.empty(), "chord settings read from pins.conf");
    }
    {   // switch wired C + NC
        FakeHal h; h.cfg = fullCfg(); h.switchNC = true;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); auto o = drain(mc);
        CHECK(o[0].cmd == CMD_ERROR && !mc.homed(), "NC switch without LIMIT_INVERT: homing refuses a fake zero");
        FakeHal h2; h2.cfg = fullCfg(); h2.switchNC = true; h2.cfg.limitInvert = true;
        MotionController mc2(h2, h2.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc2.boot(1); o = drain(mc2);
        CHECK(o[0].cmd == CMD_READY && h2.carriage <= 0, "LIMIT_INVERT = 1 homes correctly with an NC switch");
        std::ofstream("/tmp/c4_pins_inv.conf") << "LIMIT_INVERT = 1\nLIMIT_FILTER_MS = 5\nLIMIT_FILTER_MS_BAD = 1\n";
        MotionConfig c; c.load("/tmp/c4_pins_inv.conf");
        CHECK(c.limitInvert && c.limitFilterMs == 5, "LIMIT_INVERT and LIMIT_FILTER_MS read from pins.conf");
    }
    {   // blank pins: no motion at all, buttons still forwarded
        FakeHal h; h.cfg = MotionConfig(); h.cfg.btnLeft = 5;
        MotionController mc(h, h.cfg, cal, "/tmp/c4_test/servo3.conf");
        mc.boot(1); auto o = drain(mc);
        CHECK(h.steps == 0 && o[0].cmd == CMD_ERROR, "STEP/DIR/LIMIT blank: never steps, reports ERROR");
        holdBtn(h, mc, MotionHal::BTN_LEFT, 80); o = drain(mc);
        CHECK(o.size() == 1 && o[0].cmd == CMD_BUTTON, "a wired button still works without the gantry pins");
        for (int i = 0; i < 5; i++) { h.t += 3100000; mc.tick(); }
        CHECK(mc.autoHomeTries() == 0, "no automatic homing retries without pins");
    }

    // ================= Brain + MotionLink (the whole game path) =================
    {
        FakeHal h; h.cfg = fullCfg();
        MotionLink link(h, h.cfg, cal, "/tmp/c4_test/servo4.conf");
        Settings st; Brain brain(link, st, "/tmp/c4_test/settings.conf", 7);
        link.mc.boot(1);
        uint64_t now = 1000;
        auto run = [&](int n) { for (int i = 0; i < n; i++) { now += 20; brain.update(now); link.mc.tick(); } };
        run(20);
        CHECK(brain.state == State::MENU, "Brain reaches the menu once the Pi gantry has homed");
        CHECK(brain.startGame(0, 0), "1v1 game starts");
        run(10);
        long homesBefore = 0; h.onStep = [&](long) { if (h.carriage == 0) homesBefore++; };
        brain.onButton(BTN_CONFIRM); run(30);
        h.onStep = nullptr;
        CHECK(homesBefore > 0 && link.mc.homed() && brain.state == State::HUMAN_TURN && !brain.board.empty(),
              "first piece dropped by the real gantry code, re-homed, next player's turn");
        link.cnt = LinkCounters();
        link.send(CMD_PING, nullptr, 0);
        CHECK(link.cnt.txFrames == 1, "MotionLink counts frames for Link debug");
    }
    {   // the real thread: starts, homes, answers, stops promptly
        FakeHal h; h.cfg = fullCfg();
        MotionLink link(h, h.cfg, cal, "/tmp/c4_test/servo4.conf");
        link.start();
        bool ready = false; Frame f;
        for (int i = 0; i < 200 && !ready; i++) { while (link.poll(f, 0)) if (f.cmd == CMD_READY) ready = true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(ready, "threaded: READY arrives");
        auto t0 = std::chrono::steady_clock::now();
        link.stopThread();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1), "threaded: stops within a second");
        CHECK(h.pins[MotionHal::EN] == 1 && h.servo.back() == 0, "threaded: motor off and servo limp after stop");
    }

    printf("%d passed, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
