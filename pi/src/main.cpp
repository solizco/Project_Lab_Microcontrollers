// Game Brain entry point.
//   ./brain                            Pi 4: drives the gantry from the Pi's own GPIO (pins.conf)
//   ./brain --gpio                     same, said explicitly
//   ./brain --sim                      fake gantry, keyboard only
//   ./brain --serial /dev/serial0      old setup: ESP32 Motion Controller over the UART
// Options:
//   --pins FILE      pins file to use (default: searched, see pins.h)
//   --pins-template  print a blank pins.conf and exit
//   --check-pins     read pins.conf, claim the GPIO, report what works, and exit
//   --servo-test     move ONLY the servo through rest / red / black (game must be stopped), and exit
//                    add --soft to force software PWM for comparison
//   --faces DIR      folder with Finn's converted .face files (default: searched, see faceFolders())
//   --rotate 180     screen mounted upside down
//   --text           old colored-text UI instead of graphics
//   --board          show the small board in the corner (always on with --sim; B toggles)
// Keys: arrows or a/d = move, Enter/space = select, Esc/Backspace = back, B board, Q quit,
//       on Statistics: T adds a test game, C twice clears.
// Graphics go straight to /dev/fb0 (gfx.cpp); no SDL, X or Python needed on the Pi.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <random>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "app.h"
#include "brain.h"
#include "motion.h"
#include "pi_hal.h"
#include "pins.h"
#include <memory>
#ifndef C4_BUILD
#define C4_BUILD "dev"
#endif

static volatile sig_atomic_t g_quit = 0;
static void onSig(int) { g_quit = 1; }
static termios g_old; static bool g_have_old = false;
static void restoreTty() {
    fputs("\033[0m\033[?25h\033[2J\033[H", stdout); fflush(stdout);   // colors off, cursor on, clear
    if (g_have_old) tcsetattr(0, TCSANOW, &g_old);
}

static uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- colors (8-color set: works on the bare Linux console) ----
#define RST   "\033[0m"
#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define FG_R  "\033[31m"
#define FG_Y  "\033[33m"
#define FG_C  "\033[36m"
#define FG_W  "\033[37m"
#define BG_BL "\033[44m"   // board blue
#define BG_R  "\033[41m"   // RED piece
#define BG_W  "\033[47m"   // BLACK piece (drawn white so it shows on a dark screen)
#define BG_K  "\033[40m"   // empty hole

static const char* MODE_NAMES[4] = { "PASS & PLAY", "NOOB", "PRO", "HACKER" };
static const char* MODE_BLURB[4] = { "two humans, no AI", "Roger plays loose", "Roger plays sharp", "Roger plays perfect" };

static const char* faceText(FaceEvent f) {
    switch (f) {
    case FACE_THINKING:  return "[ o_O ]  hmm...";
    case FACE_GOOD_MOVE: return "[ >_< ]  ...nice move.";
    case FACE_BAD_MOVE:  return "[ ^_^ ]  heh. I see something.";
    case FACE_WIN:       return "[ ^o^ ]  I WIN!";
    case FACE_LOSS:      return "[ T_T ]  you got me.";
    default:             return "[ o_o ]";
    }
}

static std::string render(const Brain& b, const std::deque<std::string>& logs, bool sim, uint64_t now, uint64_t ai_start) {
    std::string o = "\033[H";   // home; lines are cleared as they are written (no flicker)
    char buf[256];

    o += BOLD FG_C "  CONNECT FOUR  vs  ROGER" RST;
    if (b.state != State::HOMING && b.state != State::MENU) { o += "   "; o += DIM; o += MODE_NAMES[b.mode]; o += RST; }
    o += sim ? DIM "   [sim]" RST "\n\n" : "\n\n";

    if (b.state == State::HOMING) {
        o += "  Homing gantry... waiting for the Motion Controller.\n";
    } else if (b.state == State::MENU) {
        if (!b.inConfirm) {
            o += "  Choose a mode:\n\n";
            for (int i = 0; i < 4; i++) {
                snprintf(buf, sizeof buf, "   %s %-12s" RST "  " DIM "%s" RST "\n\n",
                         i == b.menuIndex ? BOLD FG_Y ">" : " ", MODE_NAMES[i], MODE_BLURB[i]);
                o += buf;
            }
            o += DIM "  left / right to choose, Enter to select" RST "\n";
        } else {
            snprintf(buf, sizeof buf, "  You selected  " BOLD FG_Y "%s" RST "\n  " DIM "%s" RST "\n\n",
                     MODE_NAMES[b.menuIndex], MODE_BLURB[b.menuIndex]);
            o += buf;
            o += b.confirmIndex == 0 ? BOLD FG_Y "   > START GAME" RST "\n\n" : "     START GAME\n\n";
            o += b.confirmIndex == 1 ? BOLD FG_Y "   > BACK" RST "\n" : "     BACK\n";
        }
    } else if (b.state == State::FAULT) {
        snprintf(buf, sizeof buf, "\n  " BOLD "\033[41m\033[37m  MOTION FAULT  code 0x%02X  " RST "\n\n", b.faultCode);
        o += buf;
        const char* why = b.faultCode == 0xFF ? "Boards out of sync (wrong column confirmed)." :
                          b.faultCode == 0xFD ? "No reply after a drop. Check whether the piece actually fell." :
                          b.faultCode == 0x01 ? "Stepper stall or homing failed." :
                          b.faultCode == 0x04 ? "Drop requested before homing." : "Motion Controller error.";
        o += "  "; o += why; o += "\n  Check the mechanism, then power-cycle.\n";
    } else {
        // selector row
        o += "  ";
        for (int c = 0; c < 7; c++)
            o += (b.state == State::HUMAN_TURN && c == b.selectedCol) ? BOLD FG_Y "  vv  " RST : "      ";
        o += "\n";
        // board: each cell is 6 wide x 2 tall
        o += "  " BG_BL; o += std::string(42, ' '); o += RST "\n";
        for (int r = 5; r >= 0; r--) {
            for (int line = 0; line < 2; line++) {
                o += "  ";
                for (int c = 0; c < 7; c++) {
                    int v = b.board.at(c, r);
                    o += BG_BL " ";
                    o += v == 0 ? BG_R : v == 1 ? BG_W : BG_K;
                    o += "    ";
                    o += BG_BL " ";
                }
                o += RST "\n";
            }
        }
        o += "  " BG_BL; o += std::string(42, ' '); o += RST "\n  ";
        for (int c = 0; c < 7; c++) { snprintf(buf, sizeof buf, "  %d   ", c + 1); o += buf; }
        o += "\n\n";

        // status
        o += "  ";
        bool ai = b.mode != 0;
        switch (b.state) {
        case State::HUMAN_TURN:
            if (ai) o += BOLD FG_W "Your turn" RST "  (you are BLACK, shown as white)";
            else    o += b.board.toMove() == 0 ? BOLD FG_R "RED's turn" RST : BOLD FG_W "BLACK's turn" RST;
            break;
        case State::AI_TURN:
            snprintf(buf, sizeof buf, BOLD FG_R "Roger is thinking..." RST "  %.1f s", (now - ai_start) / 1000.0);
            o += buf; break;
        case State::AWAITING_DROP: o += "Dropping piece..."; break;
        case State::GAME_OVER:
            if (b.winner == 2)      o += BOLD "DRAW." RST;
            else if (!ai)           o += b.winner == 0 ? BOLD FG_R "RED WINS!" RST : BOLD FG_W "BLACK WINS!" RST;
            else                    o += b.winner == 0 ? BOLD FG_R "ROGER WINS." RST : BOLD FG_Y "YOU WIN!" RST;
            o += "   Enter to play again";
            break;
        default: break;
        }
        o += "\n";
        if (ai) { o += "  Roger: "; o += faceText(b.face); o += "\n"; }
    }

    o += "\n";
    for (const auto& l : logs) { o += DIM "  . " + l + RST "\n"; }
    o += DIM "\n  a/d or arrows = move   Enter = confirm   q = quit" RST;
    // clear the rest of each line and everything below, instead of wiping the
    // whole screen every frame (which flickers on the Linux console)
    std::string out; out.reserve(o.size() + 256);
    for (char c : o) { if (c == '\n') out += "\033[K"; out += c; }
    out += "\033[K\033[J";
    return out;
}

static void onCrash(int sig) {
    gfxRestoreConsole();
    if (g_have_old) tcsetattr(0, TCSANOW, &g_old);
    signal(sig, SIG_DFL);
    raise(sig);
}

static std::string exeDir() {
    char buf[512]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0; std::string p = buf; size_t s = p.rfind('/');
    return s == std::string::npos ? "." : p.substr(0, s);
}

// Same places Finn's player looks, plus a "roger" folder copied whole, plus next to this program.
static std::vector<std::string> faceFolders(const std::string& forced) {
    if (!forced.empty()) return { forced };
    const char* h = getenv("HOME"); std::string home = h ? h : ".";
    std::string ex = exeDir();
    return { home + "/faces", home + "/roger/faces", "/boot/firmware/faces", "/boot/firmware/roger/faces",
             "/boot/faces", "/boot/roger/faces", ex + "/faces", ex + "/roger/faces", ex + "/../faces",
             ex + "/../roger/faces", "faces", "roger/faces" };
}

// Reads one key press (handles arrow-key escape sequences). Returns false if none.
static bool readKey(Act& out) {
    char ch;
    if (read(0, &ch, 1) != 1) return false;
    switch (ch) {
    case 'q': case 'Q': out = A_QUIT; return true;
    case 'a': case 'A': case 'w': case 'W': out = A_UP; return true;
    case 'd': case 'D': case 's': case 'S': out = A_DOWN; return true;
    case '\n': case '\r': case ' ': out = A_SELECT; return true;
    case 0x7F: case 0x08: out = A_BACK; return true;
    case 't': case 'T': out = A_TEST; return true;
    case 'c': case 'C': out = A_CLEAR; return true;
    case 'b': case 'B': out = A_BOARD; return true;
    case 27: {
        char s[2] = {0, 0};
        for (int i = 0; i < 2; i++) { int tries = 0; while (read(0, &s[i], 1) != 1 && tries++ < 25) usleep(1000); }
        if (s[0] != '[' && s[0] != 'O') { out = A_BACK; return true; }      // plain Esc
        if (s[1] == 'A' || s[1] == 'D') { out = A_UP; return true; }
        if (s[1] == 'B' || s[1] == 'C') { out = A_DOWN; return true; }
        return false;
    }
    default: return false;
    }
}

// Pi supply health: bit 0 = under-voltage now, bit 16 = under-voltage since boot.
// -1 = unknown (not a Pi / no firmware interface).
static long readThrottled() {
    FILE* f = fopen("/sys/devices/platform/soc/soc:firmware/get_throttled", "r");
    char b[64] = {0};
    if (f) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); if (n) return strtol(b, nullptr, 16); }
    f = popen("vcgencmd get_throttled 2>/dev/null", "r");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b - 1, f); pclose(f);
    const char* x = n ? strstr(b, "0x") : nullptr;
    return x ? strtol(x + 2, nullptr, 16) : -1;
}

static FILE* g_logf = nullptr;   // the text UI's power-off logs here too

// Clean power-off. Tries, in order: the passwordless sudo rule that
// install_autostart.sh adds (full paths, -n = fail instead of asking for a
// password), then systemd/logind, which lets the user logged in on the screen
// power off without sudo on most Pi OS images. Every attempt and its output go
// to the log. Returns true if one was accepted; `why` gets the last error.
static bool runLogged(const char* cmd, FILE* logf, std::string& out) {
    out.clear();
    std::string full = std::string(cmd) + " 2>&1 </dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) { out = "could not start a shell"; return false; }
    char buf[256];
    while (fgets(buf, sizeof buf, p)) out += buf;
    int rc = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    if (logf) { fprintf(logf, "power-off: \"%s\" -> exit %d%s%s\n", cmd, code, out.empty() ? "" : ": ", out.c_str()); fflush(logf); }
    return code == 0;
}

static bool powerOff(FILE* logf, std::string& why) {
    const char* cmds[] = { "sudo -n /usr/sbin/shutdown -h now", "sudo -n /sbin/shutdown -h now",
                           "sudo -n /usr/bin/systemctl poweroff", "sudo -n /usr/sbin/poweroff",
                           "systemctl poweroff", "loginctl poweroff" };
    std::string out;
    for (const char* c : cmds) {
        if (runLogged(c, logf, out)) { why.clear(); return true; }
        if (!out.empty()) why = out;
    }
    if (why.empty()) why = "every power-off command was refused";
    return false;
}

// Quick check at startup, so a missing permission shows up in the log (and on
// the Shut down screen) before anyone needs it.
static bool powerOffAllowed() {
    return system("sudo -n -l /usr/sbin/shutdown -h now >/dev/null 2>&1 </dev/null") == 0 ||
           system("sudo -n -l /sbin/shutdown -h now >/dev/null 2>&1 </dev/null") == 0;
}

static int powerOffText(FILE* logf) {
    std::string why;
    if (powerOff(logf, why)) return 0;
    fprintf(stderr, "\nCould not power off: %s\n"
                    "Run install_autostart.sh again to allow it, or type: sudo shutdown now\n", why.c_str());
    return 3;   // not 0: start.sh shows this message instead of silently exiting
}

static int runText(Brain& brain, std::deque<std::string>& logs, bool sim) {
    fputs("\033[?25l\033[2J", stdout);
    std::string last;
    uint64_t ai_start = 0; State prev = brain.state;
    while (!g_quit) {
        Act a;
        while (readKey(a)) {
            if (a == A_QUIT) g_quit = 1;
            else if (a == A_UP) brain.onButton(BTN_LEFT);
            else if (a == A_DOWN) brain.onButton(BTN_RIGHT);
            else if (a == A_SELECT) brain.onButton(BTN_CONFIRM);
        }
        uint64_t now = nowMs();
        brain.update(now);
        if (brain.state == State::AI_TURN && prev != State::AI_TURN) ai_start = now;
        prev = brain.state;
        std::string frame = render(brain, logs, sim, now, ai_start);
        if (frame != last) { fputs(frame.c_str(), stdout); fflush(stdout); last = frame; }
        if (brain.shutdownRequested) {
            fputs("\n\n  Shutting down... wait for the green light to stay off.\n", stdout);
            fflush(stdout);
            return powerOffText(g_logf);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return 0;
}

int main(int argc, char** argv) {
    bool sim = false, text = false, board = false, serialMode = false, checkPins = false, servoTest = false, forceSoft = false; int rotate = 0;
    std::string dev = "/dev/serial0", facesDir, fbdev = "/dev/fb0", pinsArg;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { printf("Connect 4 brain, build %s\n", C4_BUILD); return 0; }
        else if (!strcmp(argv[i], "--sim")) sim = true;
        else if (!strcmp(argv[i], "--gpio")) serialMode = false;
        else if (!strcmp(argv[i], "--pins") && i + 1 < argc) pinsArg = argv[++i];
        else if (!strcmp(argv[i], "--pins-template")) { fputs(PINS_TEMPLATE, stdout); return 0; }
        else if (!strcmp(argv[i], "--check-pins")) checkPins = true;
        else if (!strcmp(argv[i], "--servo-test")) servoTest = true;
        else if (!strcmp(argv[i], "--soft")) forceSoft = true;
        else if (!strcmp(argv[i], "--text")) text = true;
        else if (!strcmp(argv[i], "--board")) board = true;
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc) { dev = argv[++i]; serialMode = true; }
        else if (!strcmp(argv[i], "--faces") && i + 1 < argc) facesDir = argv[++i];
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) rotate = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--gpio | --sim | --serial DEVICE] [--pins FILE] [--faces DIR] [--rotate 0|180] [--text] [--board]\n", argv[0]);
            return 2;
        }
    }
    if (rotate != 0 && rotate != 180) { fprintf(stderr, "--rotate must be 0 or 180 (tall layouts are not supported)\n"); return 2; }

    // log to ~/c4_log.txt (the screen is busy with graphics) and keep the last lines for --text
    const char* h = getenv("HOME");
    std::string logPath = std::string(h ? h : ".") + "/c4_log.txt";
    FILE* logf = fopen(logPath.c_str(), "w");
    g_logf = logf;
    auto t0 = nowMs();

    SimLink simlink; simlink.debug_frames = true; SerialLink serial; Link* link = &simlink;
    simlink.home_ms = 800; simlink.drop_ms = 1200;   // snappier for playing at the desk
    // ---- the gantry on the Pi's own GPIO (Pi 4, no ESP32) ----
    // Declared in this order so the motion thread stops before the GPIO is released.
    std::unique_ptr<PiHal> piHal;
    std::unique_ptr<MotionLink> gpioLink;
    MotionConfig mcfg;
    std::string gantryInfo, gantryProblem;
    bool gpioMode = !sim && !serialMode;
    if (gpioMode) {
        char exe[512] = {0}; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string exeDir = n > 0 ? std::string(exe, (size_t)n) : std::string(".");
        exeDir = exeDir.substr(0, exeDir.rfind('/') == std::string::npos ? 0 : exeDir.rfind('/'));
        std::string pinsPath = pinsArg.empty() ? MotionConfig::find(exeDir) : pinsArg;
        if (pinsPath.empty()) {                    // first run: leave a blank file to fill in
            pinsPath = std::string(h ? h : ".") + "/roger/pins.conf";
            if (FILE* pf = fopen(pinsPath.c_str(), "wx")) { fputs(PINS_TEMPLATE, pf); fclose(pf); }
        }
        if (!mcfg.load(pinsPath)) gantryProblem = "cannot read " + pinsPath;
        if (servoTest) {
            printf("pins file: %s\nSERVO = %s\n", pinsPath.c_str(), mcfg.servo >= 0 ? ("GPIO" + std::to_string(mcfg.servo)).c_str() : "(blank)");
            if (mcfg.servo < 0) { printf("Fill in SERVO in pins.conf first.\n"); return 1; }
            MotionConfig only; only.servo = mcfg.servo; only.servoForceSoftware = forceSoft || mcfg.servoForceSoftware;
            only.servoMinUs = mcfg.servoMinUs; only.servoMaxUs = mcfg.servoMaxUs;
            PiHal hal;
            if (!hal.open(only)) {
                printf("FAILED to set up the servo pin: %s\n(quit the game first with Q: it owns the pins while it runs)\n", hal.error().c_str());
                return 1;
            }
            ServoCal c; loadServoCal(servoCalPath(), c);
            printf("servo output: %s\n", hal.servoInfo().c_str());
            printf("Watch the servo. Each step holds 1.5 s.\n");
            struct { const char* name; int deg; } seq[] = {{"rest", c.rest}, {"red select", c.redSel}, {"red push", c.redPush},
                {"rest", c.rest}, {"black select", c.blackSel}, {"black push", c.blackPush}, {"rest", c.rest}};
            for (auto& st : seq) {
                int us = only.servoMinUs + (only.servoMaxUs - only.servoMinUs) * st.deg / 180;
                hal.servoUs(us);
                printf("  %-12s %3d deg = %4d us   -> %s\n", st.name, st.deg, us, hal.servoReport().c_str());
                hal.sleepMs(150);
                std::string sig = hal.servoSignalCheck(true);
                printf("  %-12s measured on the pin: %s\n", "", sig.empty() ? "(cannot measure on this Pi)" : sig.c_str());
                fflush(stdout);
                hal.sleepMs(1350);
            }
            hal.servoUs(0);
            printf("done (servo limp). Pulse-width errors fixed along the way: %d\n", hal.servoErrors());
            printf("If every line says verified/on but the servo did not move: it's power or ground.\n"
                   "  servo red to brown = 5-6 V, servo brown joined to a Pi GND, signal on the right pin.\n");
            return 0;
        }
        if (checkPins) {
            printf("pins file: %s\n%s\n", pinsPath.c_str(), mcfg.summary().c_str());
            for (auto& p : mcfg.problems) printf("  PROBLEM: %s\n", p.c_str());
            if (!mcfg.motionReady()) { std::string m; for (auto& x : mcfg.missing()) m += " " + x; printf("  still blank (needed to move):%s\n", m.c_str()); }
            PiHal hal;
            if (!hal.open(mcfg)) { printf("GPIO: FAILED: %s\n", hal.error().c_str()); return 1; }
            printf("GPIO: OK on %s\nservo: %s\n", hal.chipInfo().c_str(), hal.servoInfo().c_str());
            const char* nm[MotionHal::N_LINES] = {"STEP", "DIR", "EN", "LIMIT", "BTN_LEFT", "BTN_RIGHT", "BTN_SELECT", "BTN_SHUTDOWN"};
            for (int l = MotionHal::LIMIT; l < MotionHal::N_LINES; l++) {
                if (!hal.wired((MotionHal::Line)l)) continue;
                bool pressed = hal.read((MotionHal::Line)l) == 0;
                if (l == MotionHal::LIMIT && mcfg.limitInvert) pressed = !pressed;
                printf("  %-12s reads %s\n", nm[l], pressed ? "PRESSED" : "open / released");
            }
            if (mcfg.limit >= 0) {          // the switch is where motor noise shows up: sample it for 2 s
                int lows = 0;
                for (int i = 0; i < 2000; i++) { if ((hal.read(MotionHal::LIMIT) == 0) != mcfg.limitInvert) lows++; hal.sleepMs(1); }
                printf("  LIMIT over 2 s: pressed in %d of 2000 readings%s\n", lows,
                       lows == 0 ? "" : lows == 2000 ? " (held the whole time)" : "  <-- FLICKERING: noise or a loose wire");
            }
            printf("Press buttons or the limit switch and run this again to see them change.\n");
            return mcfg.motionReady() && mcfg.problems.empty() ? 0 : 1;
        }
        if (logf) {
            fprintf(logf, "gantry: Pi GPIO, pins from %s\ngantry: %s\n", pinsPath.c_str(), mcfg.summary().c_str());
            for (auto& p : mcfg.problems) fprintf(logf, "gantry: pins.conf problem: %s\n", p.c_str());
            fflush(logf);
        }
        ServoCal cal; std::string calPath = servoCalPath();
        loadServoCal(calPath, cal);
        piHal.reset(new PiHal());
        if (!piHal->open(mcfg)) {
            if (gantryProblem.empty()) gantryProblem = piHal->error();
            if (logf) { fprintf(logf, "gantry: GPIO NOT available: %s\n", piHal->error().c_str()); fflush(logf); }
            piHal.reset(new PiHal());              // nothing claimed: the game still runs, on the screen
            mcfg.step = mcfg.dir = mcfg.limit = -1;  // nothing to drive: skip homing entirely
        } else if (logf) { fprintf(logf, "gantry: GPIO chip %s\n", piHal->chipInfo().c_str()); fflush(logf); }
        gpioLink.reset(new MotionLink(*piHal, mcfg, cal, calPath));
        gpioLink->mc.log = [logf, t0](const std::string& m) {
            if (logf) { fprintf(logf, "%8.2f gantry: %s\n", (nowMs() - t0) / 1000.0, m.c_str()); fflush(logf); }
        };
        if (gantryProblem.empty() && !mcfg.motionReady()) {
            std::string m; for (auto& x : mcfg.missing()) m += (m.empty() ? "" : ", ") + x;
            gantryProblem = m + " blank in " + pinsPath;
        }
        gantryInfo = gantryProblem.empty() ? "Gantry on Pi GPIO, pins from " + pinsPath
                                           : "Gantry NOT ready: " + gantryProblem;
        if (!mcfg.problems.empty())
            gantryInfo += "  (" + std::to_string(mcfg.problems.size()) + " pins.conf problem" + (mcfg.problems.size() > 1 ? "s" : "") + ": NOT all settings used)";
        link = gpioLink.get();
    } else if (serialMode && !sim) {
        if (!serial.open(dev)) { fprintf(stderr, "cannot open serial port: %s\n(use --sim to run without hardware)\n", serial.error().c_str()); return 1; }
        link = &serial;
    }

    Settings settings; std::string spath = Settings::defaultPath();
    settings.load(spath);
    // Screen-only play (Options > Screen only) swaps the gantry for an on-screen
    // simulator while the ESP32's buttons keep working. It lasts until power-off:
    // every start is normal gantry mode with the Motion Controller buttons, so the
    // Pi can never come up stuck in a mode nobody can control.
    if (settings.screen_only || settings.screen_input != 1) {
        settings.screen_only = false;
        settings.screen_input = 1;
        settings.save(spath);
    }
    SwitchLink sw(*link);
    sw.screenOnly = false;
    Brain brain(sw, settings, spath, std::random_device{}());
    if (gpioLink) gpioLink->start();           // homes now; Brain's HOME is already queued

    std::deque<std::string> logs;
    brain.log = [&logs, logf, t0](const std::string& m) {
        logs.push_back(m); while (logs.size() > 4) logs.pop_front();
        if (logf) { fprintf(logf, "%8.2f %s\n", (nowMs() - t0) / 1000.0, m.c_str()); fflush(logf); }
    };

    signal(SIGINT, onSig); signal(SIGTERM, onSig);
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) signal(s, onCrash);
    if (isatty(0) && tcgetattr(0, &g_old) == 0) {
        g_have_old = true;
        termios t = g_old; t.c_lflag &= ~(ICANON | ECHO); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    }
    atexit(restoreTty);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    Display display; std::string info;
    if (!text && !display.open(fbdev, info)) {
        fprintf(stderr, "graphics unavailable (%s), using the text UI\n", info.c_str());
        text = true;
    }
    if (logf) fprintf(logf, "build %s\n", C4_BUILD);
    if (logf && !text) fprintf(logf, "display: %s\n", info.c_str());
    if (text) return runText(brain, logs, sim);

    FacePlayer faces;
    std::string faceMsg;
    faces.load(faceFolders(facesDir), faceMsg);
    if (logf) { fprintf(logf, "faces: %s\n", faceMsg.c_str());
                std::string n; for (auto& g : faces.names()) n += " " + g; fprintf(logf, "face groups:%s\n", n.c_str()); fflush(logf); }

    Stats stats; std::string statsPath = Stats::defaultPath();
    stats.load(statsPath);

    App app(brain, faces, stats, statsPath, sim, &sw);
    app.faceMessage = faceMsg;
    if (board) app.showBoard = true;
    brain.buttonHook = [&app](uint8_t b) { return app.espButton(b); };
    if (gpioLink) {
        app.setGantry(&gpioLink->mc);
        app.gantryInfo = gantryInfo;
        app.chordSelect = mcfg.chordSelect && mcfg.btnLeft >= 0 && mcfg.btnRight >= 0;
        if (!gantryProblem.empty()) {
            // No working gantry: play on the screen so the Pi is never stuck. Buttons if
            // they are wired, otherwise the keyboard. Not saved: every start tries again.
            bool buttons = mcfg.btnLeft >= 0 && mcfg.btnRight >= 0 && (mcfg.btnSelect >= 0 || mcfg.chordSelect);
            app.setScreenMode(true, buttons ? 1 : 0);
            app.showBanner("Gantry not ready: " + gantryProblem, "Playing on the screen only. Fix pins.conf on the SD card, then restart.",
                           nowMs(), 15000);
        } else if (!mcfg.problems.empty()) {
            app.showBanner("pins.conf: " + mcfg.problems[0],
                           std::to_string(mcfg.problems.size()) + " problem(s), details in ~/c4_log.txt. Fix it on the SD card, then restart.",
                           nowMs(), 15000);
        }
    }

    bool canOff = powerOffAllowed();
    if (logf) { fprintf(logf, "power-off without a password: %s\n", canOff ? "allowed" : "NOT allowed (run install_autostart.sh)"); fflush(logf); }
    app.powerOffAllowed = canOff;
    uint64_t shutdownRetryAt = 0;
    long throttled = readThrottled();
    uint64_t throttleCheckAt = nowMs() + 5000, lastPowerBanner = 0;
    if (logf) { fprintf(logf, "power: get_throttled = 0x%lx%s\n", throttled < 0 ? 0 : throttled,
                        throttled < 0 ? " (unknown)" : (throttled & 0x10000) ? " UNDER-VOLTAGE has happened since boot" : " ok"); fflush(logf); }

    Canvas cv(800, 480);
    int perfFrames = 0; double perfMs = 0, perfMax = 0; uint64_t perfStart = nowMs();
    while (!g_quit && !app.quit) {
        Act a;
        while (readKey(a)) app.key(a);
        uint64_t now = nowMs();
        brain.update(now);
        auto f0 = std::chrono::steady_clock::now();
        if (app.frame(now, cv)) {
            display.present(cv, rotate == 180);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            perfFrames++; perfMs += ms; if (ms > perfMax) perfMax = ms;
        }
        if (logf && now - perfStart >= 10000) {     // every 10 s: how expensive drawing is on this Pi
            if (perfFrames) fprintf(logf, "ui: %d frames in 10 s, %.1f ms avg, %.1f ms worst (decode+draw+present)\n",
                                    perfFrames, perfMs / perfFrames, perfMax);
            fflush(logf); perfFrames = 0; perfMs = perfMax = 0; perfStart = now;
        }
        if (now >= throttleCheckAt) {      // watch the Pi's own supply: a sagging 5 V rail looks like random faults
            throttleCheckAt = now + 5000;
            long t2 = readThrottled();
            if (t2 >= 0 && t2 != throttled) {
                if (logf) { fprintf(logf, "%8.2f power: get_throttled 0x%lx -> 0x%lx%s\n", (now - t0) / 1000.0, throttled < 0 ? 0 : throttled, t2,
                                    (t2 & 1) ? "  UNDER-VOLTAGE NOW" : ""); fflush(logf); }
                throttled = t2;
            }
            if (t2 > 0 && (t2 & 1) && now - lastPowerBanner > 60000) {
                lastPowerBanner = now;
                app.showBanner("Under-voltage: the Pi's 5 V supply is sagging",
                               "Often the servo or motor sharing its supply. Give the Pi its own 5 V 3 A.", now, 10000);
            }
        }
        if (app.shutdownRequested && now < shutdownRetryAt) {      // button still held after a failed try
            app.shutdownRequested = false; brain.shutdownRequested = false;
        }
        if (app.shutdownRequested) {
            if (logf) { fprintf(logf, "shutdown requested (%s)\n", brain.shutdownRequested ? "Motion Controller button" : "menu"); fflush(logf); }
            app.drawShuttingDown(cv);
            display.present(cv, rotate == 180);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            std::string why;
            if (gpioLink) gpioLink->stopThread();   // motor and servo off before the power goes
            if (powerOff(logf, why)) {
                // The system is going down: leave "Shutting down..." on the screen
                // until systemd stops us (SIGTERM ends the loop below).
                for (int i = 0; i < 3000 && !g_quit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                display.close();
                if (logf) fclose(logf);
                return 0;
            }
            // Refused: say why and keep the game running, so the buttons still work.
            if (gpioLink) gpioLink->start();
            app.shutdownRequested = false; brain.shutdownRequested = false;
            shutdownRetryAt = nowMs() + 5000;
            app.showBanner("Could not power off: " + why, "Fix: run install_autostart.sh again (see ~/c4_log.txt).", nowMs(), 10000);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    display.close();
    if (logf) fclose(logf);
    return 0;
}
