#include "brain.h"
#include <cstdio>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

const char* stateName(State s) {
    switch (s) {
    case State::HOMING:        return "HOMING";
    case State::MENU:          return "MENU";
    case State::HUMAN_TURN:    return "HUMAN_TURN";
    case State::AI_TURN:       return "AI_TURN";
    case State::AWAITING_DROP: return "AWAITING_DROP";
    case State::GAME_OVER:     return "GAME_OVER";
    case State::FAULT:         return "FAULT";
    }
    return "?";
}

Brain::Brain(Link& link, Settings settings, std::string settings_path, uint32_t rng_seed)
    : link_(link), settings_(settings), settings_path_(std::move(settings_path)), rng_(rng_seed) {
    menuIndex = settings_.last_mode;
    sendHome();        // tell the ESP32 we're up; it also beacons READY until it hears us
}

Brain::~Brain() { stopSearch(); }

// ---------------- link debugging ----------------
static const char* cmdName(uint8_t c) {
    switch (c) {
    case CMD_HOME: return "HOME"; case CMD_DROP: return "DROP"; case CMD_SELECTOR: return "SELECTOR";
    case CMD_BUTTON: return "BUTTON"; case CMD_READY: return "READY"; case CMD_DONE: return "DONE";
    case CMD_ERROR: return "ERROR"; case CMD_SHUTDOWN: return "SHUTDOWN"; case CMD_PING: return "PING";
    case CMD_RESET: return "RESET"; case CMD_DEBUG: return "DEBUG";
    }
    return "?";
}

const char* Brain::resetReasonName(int r) {
    switch (r) {
    case 1: return "power-on"; case 2: return "reset pin"; case 3: return "software restart";
    case 4: return "crash"; case 5: case 6: case 7: return "watchdog"; case 8: return "deep sleep";
    case 9: return "BROWNOUT (power dip)";
    }
    return "unknown";
}

void Brain::logLink(bool fromMc, const std::string& s) {
    linkLog.push_back({now_, fromMc, s});
    while (linkLog.size() > LINK_LOG_MAX) linkLog.pop_front();
    say(std::string(fromMc ? "MC > Pi: " : "Pi > MC: ") + s);
}

void Brain::tx(uint8_t cmd, const uint8_t* p, uint8_t n, bool hardware) {
    if (hardware) link_.sendToHardware(cmd, p, n); else link_.send(cmd, p, n);
    char b[64];
    if (cmd == CMD_DROP && n >= 2) snprintf(b, sizeof b, "DROP col %d %s", p[0], p[1] ? "BLACK" : "RED");
    else if (cmd == CMD_SELECTOR && n >= 1) snprintf(b, sizeof b, "SELECTOR col %d", p[0]);
    else snprintf(b, sizeof b, "%s", cmdName(cmd));
    logLink(false, b);
}

std::string Brain::describe(const Frame& f) const {
    char b[96];
    const uint8_t* p = f.payload;
    switch (f.cmd) {
    case CMD_BUTTON: snprintf(b, sizeof b, "BUTTON %s", p[0] == 0 ? "left" : p[0] == 1 ? "right" : "select"); break;
    case CMD_DONE:   snprintf(b, sizeof b, "DONE col %d", p[0]); break;
    case CMD_ERROR:  snprintf(b, sizeof b, "ERROR 0x%02X", p[0]); break;
    case CMD_DEBUG:
        switch (p[0]) {
        case DBG_BOOT:   snprintf(b, sizeof b, "restarted (%s), firmware v%d", resetReasonName(p[1]), p[2]); break;
        case DBG_RX:     snprintf(b, sizeof b, "heard %s %d %d", cmdName(p[1]), p[2], p[3]); break;
        case DBG_MOVE:   snprintf(b, sizeof b, p[2] ? "arrived at col %d" : "moving to col %d", p[1]); break;
        case DBG_HOME:   snprintf(b, sizeof b, "homing %s after %d steps", p[1] ? "OK" : "FAILED", (p[2] << 8) | p[3]); break;
        case DBG_SERVO:  snprintf(b, sizeof b, "servo dropped %s", p[1] ? "BLACK" : "RED"); break;
        case DBG_BAD:    snprintf(b, sizeof b, "got a CORRUPTED frame (%d total)", (p[1] << 8) | p[2]); break;
        case DBG_STATUS: snprintf(b, sizeof b, "status: %s, limit %s, fw v%d, bad frames %d",
                                  (p[1] & 1) ? "homed" : "NOT homed", (p[1] & 2) ? "pressed" : "open", p[2], p[3]); break;
        default:         snprintf(b, sizeof b, "debug event %d", p[0]); break;
        }
        break;
    default: snprintf(b, sizeof b, "%s", cmdName(f.cmd)); break;
    }
    return b;
}

void Brain::restartMotionController() {
    if (!debugAllowed()) return;
    stopSearch(); faultCode = 0; winner = -1; inConfirm = false; setFace(FACE_IDLE);
    tx(CMD_RESET, nullptr, 0, true);
    strayReady_ = 0;
    enter(State::HOMING);        // the board reboots, homes and beacons READY
    restartSentAt_ = now_ ? now_ : 1;
}

bool Brain::debugPing() {
    if (!debugAllowed()) return false;
    tx(CMD_PING, nullptr, 0, true);
    return true;
}

bool Brain::debugMove(int col) {
    if (!debugAllowed() || col < 0 || col > 6) return false;
    uint8_t p[1] = { (uint8_t)col };
    tx(CMD_SELECTOR, p, 1, true);
    return true;
}

bool Brain::debugDrop(int col, int color) {
    if (!debugAllowed() || col < 0 || col > 6) return false;
    uint8_t p[2] = { (uint8_t)col, (uint8_t)(color ? 1 : 0) };
    tx(CMD_DROP, p, 2, true);    // reply is logged; the game board is not touched
    return true;
}

// ---------------- helpers ----------------
void Brain::enter(State s) {
    state = s;
    state_since_ = now_;
    char b[64]; snprintf(b, sizeof b, "-> %s", stateName(s)); say(b);
}

void Brain::sendHome() {
    restartSentAt_ = 0;
    tx(CMD_HOME, nullptr, 0);
    strayReady_ = 0;
    enter(State::HOMING);
}

void Brain::sendDrop(int col, int mover) {
    uint8_t p[2] = { (uint8_t)col, (uint8_t)mover };
    tx(CMD_DROP, p, 2);
    pending_col_ = col; pending_mover_ = mover;
    enter(State::AWAITING_DROP);
}

void Brain::sendSelector() {
    uint8_t p[1] = { (uint8_t)selectedCol };
    tx(CMD_SELECTOR, p, 1);
}

void Brain::fault(uint8_t code, const char* why) {
    stopSearch();
    faultFromHoming_ = (state == State::HOMING);
    faultCode = code;
    setFace(FACE_IDLE);
    enter(State::FAULT);
    char b[128]; snprintf(b, sizeof b, "FAULT 0x%02X: %s", code, why); say(b);
}

int Brain::nearestLegal(int start) const {
    if (board.legal(start)) return start;
    for (int off = 1; off < C4_WIDTH; off++) {
        if (start + off < C4_WIDTH && board.legal(start + off)) return start + off;
        if (start - off >= 0       && board.legal(start - off)) return start - off;
    }
    return start;
}

// ---------------- game flow ----------------
void Brain::beginGame(int m, int first) {
    stopSearch();
    mode = m;
    settings_.last_mode = (uint8_t)m;
    settings_.save(settings_path_);          // best effort; failure is not fatal
    board.reset(first);
    winner = -1;
    reaction_.reset();
    inConfirm = false;
    say(m == 0 ? "Mode: PASS & PLAY" : (std::string("Mode: ") + difficultyFor((uint8_t)m)->name));
    if (m == 0) say(first ? "Player Two (BLACK) goes first" : "Player One (RED) goes first");
    else        say(first ? "the human (BLACK) goes first" : "R.O.G.E.R (RED) goes first");
    setFace(FACE_IDLE);
    if (m != 0 && first == 0) beginAiTurn(); else beginHumanTurn();
}

// The Motion Controller re-homes (re-zeros on the limit switch) after EVERY
// drop, in every mode, so drift never builds up. Then the human turn starts
// and the gantry drives out to the selected column.
void Brain::beginHumanTurn() {
    selectedCol = nearestLegal(0);           // gantry is at the limit switch after every drop
    setFace(FACE_IDLE);                      // a reaction still playing finishes its pass first (App)
    enter(State::HUMAN_TURN);
    sendSelector();
}

// R.O.G.E.R's own search starts the moment the human confirms a drop, on the
// position after that move, while the gantry is still moving, so his move is
// normally ready the instant the piece lands.
void Brain::beginAiTurn() {
    enter(State::AI_TURN);
    if (resultReady_) { playAiMove(); return; }        // decided during the drop
    if (face != FACE_GOOD_MOVE && face != FACE_BAD_MOVE) setFace(FACE_THINKING);   // a reaction keeps playing
    if (!searching_) startSearch(board, false);
}

void Brain::startSearch(const Board& position, bool reactToHumanMove) {
    stopSearch();                            // never two searches
    cancel_ = false; done_ = false; searching_ = true;
    resultReady_ = false; reactPending_ = reactToHumanMove;
    searchedState_ = position.state();
    Board snapshot = position;               // the thread searches a COPY
    const DifficultyConfig cfg = *difficultyFor((uint8_t)mode);
    uint32_t seed = rng_();
    worker_ = std::thread([this, snapshot, cfg, seed] {
#ifdef __linux__
        // Lower priority than the UI/UART thread so the screen and serial
        // stay responsive on the single-core Zero. Raising niceness needs no root.
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
#endif
        std::mt19937 trng(seed);
        result_ = findBestMove(snapshot, cfg, cancel_, trng);
        done_ = true;
    });
}

void Brain::stopSearch() {
    if (worker_.joinable()) { cancel_ = true; worker_.join(); }
    searching_ = false;
    resultReady_ = false;
    reactPending_ = false;
}

void Brain::harvestSearch() {
    if (worker_.joinable()) worker_.join();
    searching_ = false;
    reactPending_ = false;
    if (result_.aborted) { resultReady_ = false; return; }
    resultReady_ = true;
    lastAiValue = result_.value; lastAiNodes = result_.nodes;
}

void Brain::playAiMove() {
    resultReady_ = false;
    const C4State& now = board.state();
    bool samePosition = now.bitboard[0] == searchedState_.bitboard[0] && now.bitboard[1] == searchedState_.bitboard[1];
    if (!samePosition) { startSearch(board, false); return; }   // stale: think again
    if (!board.legal(result_.move)) { fault(FAULT_DESYNC, "AI returned no legal move"); return; }
    char b[96]; snprintf(b, sizeof b, "AI chose col %d value=%d nodes=%u", result_.move, result_.value, result_.nodes);
    say(b);
    if (face == FACE_THINKING) setFace(FACE_IDLE);     // done thinking (a reaction, if any, keeps playing)
    sendDrop(result_.move, board.toMove());
}

void Brain::onDone(int col) {
    if (col != pending_col_) { fault(FAULT_DESYNC, "CMD_DONE column mismatch"); return; }
    int mover = pending_mover_;
    board.play(pending_col_);
    bool won = board.won(mover);
    if (won || board.full()) {
        stopSearch();
        winner = won ? mover : 2;
        if (won && mode != 0) setFace((mover == 0) ? FACE_WIN : FACE_LOSS);    // RED is the AI
        enter(State::GAME_OVER);
        say(won ? (mover == 0 ? "RED wins" : "BLACK wins") : "Draw");
        return;
    }
    if (mode == 0 || mover == 0) beginHumanTurn();   // Pass&Play: always human; else AI(RED) just moved
    else                          beginAiTurn();
}

void Brain::rehome() {
    stopSearch();
    faultCode = 0;
    winner = -1;
    inConfirm = false;
    autoRestartsLeft_ = AUTO_RESTARTS;
    setFace(FACE_IDLE);
    sendHome();
}

bool Brain::startGame(int m, int first) {
    if (state != State::MENU || m < 0 || m > 3) return false;
    menuIndex = m;
    beginGame(m, first ? 1 : 0);
    return true;
}

// ---------------- input ----------------
void Brain::onButton(uint8_t btn) {
    switch (state) {
    case State::MENU:
        if (!inConfirm) {
            if (btn == BTN_LEFT)  menuIndex = (menuIndex + 3) % 4;
            if (btn == BTN_RIGHT) menuIndex = (menuIndex + 1) % 4;
            if (btn == BTN_CONFIRM) { inConfirm = true; confirmIndex = 0; }
        } else {
            if (btn == BTN_LEFT || btn == BTN_RIGHT) confirmIndex ^= 1;
            if (btn == BTN_CONFIRM) {
                if (confirmIndex == 0) beginGame(menuIndex);
                else inConfirm = false;
            }
        }
        break;

    case State::HUMAN_TURN:
        if (btn == BTN_LEFT) {
            for (int c = selectedCol - 1; c >= 0; c--)
                if (board.legal(c)) { selectedCol = c; sendSelector(); break; }
        } else if (btn == BTN_RIGHT) {
            for (int c = selectedCol + 1; c < C4_WIDTH; c++)
                if (board.legal(c)) { selectedCol = c; sendSelector(); break; }
        } else if (btn == BTN_CONFIRM && board.legal(selectedCol)) {
            int col = selectedCol, mover = board.toMove();
            Board after = board; after.play(col);
            sendDrop(col, mover);
            if (mode != 0 && !after.won(mover) && !after.full()) {
                // R.O.G.E.R reacts the moment the drop is pressed: a quick look at the
                // position decides good or bad; the reaction loops until the
                // player's next turn. Then his real search starts on that position.
                stopSearch();                          // the search code allows one search at a time
                SearchResult q = h_alphabeta_search(after.state(), 6);
                FaceEvent f = ReactionClassifier::judge(q.value);
                char b[96]; snprintf(b, sizeof b, "judged the human's move: value=%d -> %s", q.value,
                                     f == FACE_GOOD_MOVE ? "good-move reaction" : "bad-move reaction");
                say(b);
                setFace(f);
                startSearch(after, false);
            }
        }
        break;

    case State::GAME_OVER:
        if (btn == BTN_CONFIRM) { inConfirm = false; setFace(FACE_IDLE); sendHome(); }
        break;

    default: break;   // AI turn / waiting / fault: button presses do nothing
    }
}

// ---------------- frames from the ESP32 ----------------
void Brain::handleFrame(const Frame& f) {
    logLink(true, describe(f));
    mc.lastFrame = now_;
    switch (f.cmd) {
    case CMD_READY:
        if (state == State::HOMING) {
            setFace(FACE_IDLE); enter(State::MENU);
            autoRestartsLeft_ = AUTO_RESTARTS;
            tx(CMD_PING, nullptr, 0);                // stops a boot beacon (any frame does); v2 answers with status
            break;
        }
        if (state == State::FAULT && faultFromHoming_) {
            // Homing had failed, and the board has now homed by itself (its
            // firmware keeps retrying). Back to the menu, no button needed.
            say("the gantry homed by itself after the failure: back to the menu");
            faultCode = 0; faultFromHoming_ = false; strayReady_ = 0;
            autoRestartsLeft_ = AUTO_RESTARTS;
            setFace(FACE_IDLE); enter(State::MENU);
            tx(CMD_PING, nullptr, 0);
            break;
        }
        // READY outside HOMING is the boot beacon, which only continues while
        // the Motion Controller has never heard the Pi. One can be in flight;
        // after a ping, more mean the Pi -> MC wire is not getting through.
        strayReady_++;
        if (strayReady_ == 1) tx(CMD_PING, nullptr, 0);
        else if (strayReady_ >= DEAF_READY_LIMIT && state != State::FAULT)
            fault(FAULT_DEAF, "Motion Controller keeps sending its boot READY: it is not hearing the Pi");
        break;
    case CMD_BUTTON:
        if (f.len >= 1 && !(buttonHook && buttonHook(f.payload[0]))) onButton(f.payload[0]);
        break;
    case CMD_SHUTDOWN:
        if (!shutdownRequested) say("shutdown button held on Motion Controller: powering off");
        shutdownRequested = true;
        break;
    case CMD_DEBUG:
        if (state == State::AWAITING_DROP) dropAliveAt_ = now_;   // the gantry is still working on it
        if (f.len < 4) break;
        switch (f.payload[0]) {
        case DBG_BOOT:
            mc.fw = f.payload[2]; mc.resetReason = f.payload[1]; mc.statusKnown = false; mc.badFrames = 0;
            restartSentAt_ = 0;
            strayReady_ = 0;
            if (state == State::HUMAN_TURN || state == State::AI_TURN || state == State::AWAITING_DROP)
                fault(FAULT_MC_RESET, "Motion Controller restarted in the middle of the game");
            break;
        case DBG_RX:     mc.lastHeard = now_; strayReady_ = 0; break;
        case DBG_STATUS: mc.lastHeard = now_; strayReady_ = 0; mc.statusKnown = true;
                         mc.homed = f.payload[1] & 1; mc.limit = (f.payload[1] & 2) != 0;
                         mc.fw = f.payload[2]; mc.badFrames = f.payload[3]; break;
        case DBG_HOME:   mc.homed = f.payload[1] != 0; break;
        case DBG_MOVE:   if (f.payload[2]) mc.lastMoveCol = f.payload[1]; break;
        case DBG_BAD:    mc.badFrames = (f.payload[1] << 8) | f.payload[2]; break;
        default: break;
        }
        break;
    case CMD_DONE:
        mc.lastHeard = now_; strayReady_ = 0;
        if (state == State::AWAITING_DROP && f.len >= 1) onDone(f.payload[0]);
        break;
    case CMD_ERROR:
        mc.lastHeard = now_; strayReady_ = 0;
        if (state == State::HOMING && autoRestartsLeft_ > 0) {
            // Homing failed. Pressing EN on the board fixes this on a cold start,
            // so do the same thing from here once before calling it a fault.
            autoRestartsLeft_--;
            say("homing failed: restarting the Motion Controller automatically (same as pressing EN)");
            int keep = autoRestartsLeft_;
            restartMotionController();
            autoRestartsLeft_ = keep;
            break;
        }
        if (state == State::AWAITING_DROP || state == State::HOMING)
            fault(f.len >= 1 ? f.payload[0] : 0xFF, "Motion Controller reported an error");
        break;
    default: break;
    }
}

// ---------------- main tick ----------------
void Brain::update(uint64_t now_ms) {
    now_ = now_ms;
    if (!firstNow_) firstNow_ = now_ ? now_ : 1;
    if (searching_ && done_) harvestSearch();
    Frame f;
    while (link_.poll(f, now_)) handleFrame(f);

    switch (state) {
    case State::HOMING:
        // Not one frame from the board since we started: the ESP32 is not
        // running. Try to reset it over the TX wire (works only with the 1 k
        // resistor from that wire to EN fitted; harmless without it).
        if (boardSilent() && silentBreaks_ < SILENT_BREAK_MAX &&
            (silentBreaks_ == 0 || now_ - lastSilentBreak_ >= SILENT_BREAK_EVERY_MS)) {
            silentBreaks_++; lastSilentBreak_ = now_;
            char b[96]; snprintf(b, sizeof b, "no signal from the Motion Controller: reset attempt %d over the TX wire", silentBreaks_);
            say(b);
            link_.hardwareBreak();
        }
        if (restartSentAt_ && now_ - restartSentAt_ > RESTART_FALLBACK_MS) {
            restartSentAt_ = 0;
            say("no restart report (firmware v1?): sending CMD_HOME instead");
            int keep = autoRestartsLeft_;
            sendHome();
            autoRestartsLeft_ = keep;
        } else if (now_ - state_since_ > HOME_RETRY_MS) { say("no READY yet, re-sending CMD_HOME"); sendHome(); }
        break;
    case State::AI_TURN:
        if (searching_ && done_) harvestSearch();
        if (resultReady_) playAiMove();
        break;
    case State::AWAITING_DROP:
        if (now_ - (dropAliveAt_ > state_since_ ? dropAliveAt_ : state_since_) > DROP_TIMEOUT_MS)
            fault(FAULT_TIMEOUT, "no DONE/ERROR after CMD_DROP (NOT retrying: a resend could drop a second piece)");
        break;
    default: break;
    }
}
