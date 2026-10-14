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

// ---------------- helpers ----------------
void Brain::enter(State s) {
    state = s;
    state_since_ = now_;
    char b[64]; snprintf(b, sizeof b, "-> %s", stateName(s)); say(b);
}

void Brain::sendHome() {
    link_.send(CMD_HOME, nullptr, 0);
    enter(State::HOMING);
    say("SENT CMD_HOME");
}

void Brain::sendDrop(int col, int mover) {
    uint8_t p[2] = { (uint8_t)col, (uint8_t)mover };
    link_.send(CMD_DROP, p, 2);
    pending_col_ = col; pending_mover_ = mover;
    enter(State::AWAITING_DROP);
    char b[64]; snprintf(b, sizeof b, "SENT CMD_DROP col=%d %s", col, mover == 0 ? "RED" : "BLACK"); say(b);
}

void Brain::sendSelector() {
    uint8_t p[1] = { (uint8_t)selectedCol };
    link_.send(CMD_SELECTOR, p, 1);
}

void Brain::fault(uint8_t code, const char* why) {
    stopSearch();
    faultCode = code;
    setFace(FACE_IDLE);
    enter(State::FAULT);
    char b[96]; snprintf(b, sizeof b, "FAULT 0x%02X: %s", code, why); say(b);
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
void Brain::beginGame(int m) {
    stopSearch();
    mode = m;
    settings_.last_mode = (uint8_t)m;
    settings_.save(settings_path_);          // best effort; failure is not fatal
    board.reset();
    winner = -1;
    reaction_.reset();
    inConfirm = false;
    say(m == 0 ? "Mode: PASS & PLAY" : (std::string("Mode: ") + difficultyFor((uint8_t)m)->name));
    if (m == 0) beginHumanTurn(); else beginAiTurn();   // AI (RED) always moves first
}

void Brain::beginHumanTurn() {
    selectedCol = nearestLegal(0);           // gantry is at the limit switch after every drop
    setFace(FACE_IDLE);
    enter(State::HUMAN_TURN);
    sendSelector();
}

// The AI's search usually starts EARLY: the moment the human confirms a drop,
// on the position after that move, while the gantry is still moving. That
// search's value is R.O.G.E.R's judgement of the human's move, so his
// reaction plays right after the press instead of after the gantry re-homes,
// and his own move is normally ready the instant the piece lands.
void Brain::beginAiTurn() {
    enter(State::AI_TURN);
    if (resultReady_) { playAiMove(); return; }        // decided during the drop
    setFace(FACE_THINKING);
    if (!searching_) startSearch(board, false);         // opening move: nothing to react to
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
    if (result_.aborted) { resultReady_ = false; reactPending_ = false; return; }
    resultReady_ = true;
    lastAiValue = result_.value; lastAiNodes = result_.nodes;
    if (reactPending_) {
        reactPending_ = false;
        FaceEvent f = reaction_.classify(result_.value, rng_);
        char b[96]; snprintf(b, sizeof b, "judged the human's move: value=%d -> %s", result_.value,
                             f == FACE_GOOD_MOVE ? "good-move reaction" : f == FACE_BAD_MOVE ? "bad-move reaction" : "no reaction");
        say(b);
        if (f != FACE_IDLE) setFace(f);
    }
}

void Brain::playAiMove() {
    resultReady_ = false;
    const C4State& now = board.state();
    bool samePosition = now.bitboard[0] == searchedState_.bitboard[0] && now.bitboard[1] == searchedState_.bitboard[1];
    if (!samePosition) { setFace(FACE_THINKING); startSearch(board, false); return; }   // stale: think again
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
        winner = won ? mover : 2;
        if (!won || mode == 0) setFace(FACE_IDLE);
        else setFace((mover == 0) ? FACE_WIN : FACE_LOSS);    // RED is the AI
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
    setFace(FACE_IDLE);
    sendHome();
}

bool Brain::startGame(int m) {
    if (state != State::MENU || m < 0 || m > 3) return false;
    menuIndex = m;
    beginGame(m);
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
            int col = selectedCol;
            sendDrop(col, board.toMove());
            if (mode != 0) {                              // vs R.O.G.E.R: start judging this move now
                Board after = board; after.play(col);
                startSearch(after, true);
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
    switch (f.cmd) {
    case CMD_READY:
        if (state == State::HOMING) { setFace(FACE_IDLE); enter(State::MENU); }
        break;                                       // READY anywhere else: stale beacon, ignore
    case CMD_BUTTON:
        if (f.len >= 1 && !(buttonHook && buttonHook(f.payload[0]))) onButton(f.payload[0]);
        break;
    case CMD_DONE:
        if (state == State::AWAITING_DROP && f.len >= 1) onDone(f.payload[0]);
        break;
    case CMD_ERROR:
        if (state == State::AWAITING_DROP || state == State::HOMING)
            fault(f.len >= 1 ? f.payload[0] : 0xFF, "Motion Controller reported an error");
        break;
    default: break;
    }
}

// ---------------- main tick ----------------
void Brain::update(uint64_t now_ms) {
    now_ = now_ms;
    // A finished search is harvested first, so a reaction to the human's move
    // is set before that move's DONE (which may end the game) is processed.
    if (searching_ && done_) harvestSearch();
    Frame f;
    while (link_.poll(f, now_)) handleFrame(f);

    switch (state) {
    case State::HOMING:
        if (now_ - state_since_ > HOME_RETRY_MS) { say("no READY yet, re-sending CMD_HOME"); sendHome(); }
        break;
    case State::AI_TURN:
        if (searching_ && done_) harvestSearch();
        if (resultReady_) playAiMove();
        break;
    case State::AWAITING_DROP:
        if (now_ - state_since_ > DROP_TIMEOUT_MS)
            fault(FAULT_TIMEOUT, "no DONE/ERROR after CMD_DROP (NOT retrying: a resend could drop a second piece)");
        break;
    default: break;
    }
}
