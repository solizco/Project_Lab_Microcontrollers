// Game Brain entry point.
//   ./brain --sim                      fake Motion Controller, keyboard only
//   ./brain --serial /dev/serial0      real ESP32 (its buttons work too)
// Options:
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
#include "app.h"
#include "brain.h"

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
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return 0;
}

int main(int argc, char** argv) {
    bool sim = false, text = false, board = false; int rotate = 0;
    std::string dev = "/dev/serial0", facesDir, fbdev = "/dev/fb0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = true;
        else if (!strcmp(argv[i], "--text")) text = true;
        else if (!strcmp(argv[i], "--board")) board = true;
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--faces") && i + 1 < argc) facesDir = argv[++i];
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) rotate = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--sim | --serial DEVICE] [--faces DIR] [--rotate 0|180] [--text] [--board]\n", argv[0]);
            return 2;
        }
    }
    if (rotate != 0 && rotate != 180) { fprintf(stderr, "--rotate must be 0 or 180 (tall layouts are not supported)\n"); return 2; }

    SimLink simlink; SerialLink serial; Link* link = &simlink;
    simlink.home_ms = 800; simlink.drop_ms = 1200;   // snappier for playing at the desk
    if (!sim) {
        if (!serial.open(dev)) { fprintf(stderr, "cannot open serial port: %s\n(use --sim to run without hardware)\n", serial.error().c_str()); return 1; }
        link = &serial;
    }

    Settings settings; std::string spath = Settings::defaultPath();
    settings.load(spath);
    // Screen-only play (Options > Screen only) swaps the gantry for an on-screen
    // simulator while the ESP32's buttons keep working. Remembered across reboots.
    SwitchLink sw(*link);
    sw.screenOnly = settings.screen_only;
    Brain brain(sw, settings, spath, std::random_device{}());

    // log to ~/c4_log.txt (the screen is busy with graphics) and keep the last lines for --text
    const char* h = getenv("HOME");
    std::string logPath = std::string(h ? h : ".") + "/c4_log.txt";
    FILE* logf = fopen(logPath.c_str(), "w");
    std::deque<std::string> logs;
    auto t0 = nowMs();
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
        if (app.shutdownRequested) {
            app.drawShuttingDown(cv);
            display.present(cv, rotate == 180);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            display.close();
            restoreTty();
            if (logf) fclose(logf);
            // Full paths: the passwordless rule install_autostart.sh adds only covers these exact
            // commands. -n = never ask for a password (fail instead of hanging on a hidden prompt).
            const char* cmds[] = { "sudo -n /usr/sbin/shutdown -h now", "sudo -n /sbin/shutdown -h now",
                                   "systemctl poweroff 2>/dev/null" };
            int rc = -1;
            for (const char* c : cmds) { rc = system(c); if (rc == 0) break; }
            if (rc != 0) {
                fprintf(stderr, "\nCould not power off without a password.\n"
                                "Run install_autostart.sh again to allow it, or type: sudo shutdown now\n");
                return 3;   // not 0: start.sh shows this message instead of silently exiting
            }
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    display.close();
    if (logf) fclose(logf);
    return 0;
}
