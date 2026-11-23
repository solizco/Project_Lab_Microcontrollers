#include "app.h"
#include "motion.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <random>
#ifndef C4_BUILD
#define C4_BUILD "dev"
#endif

// Finn's colors
static const uint16_t BLACK = rgb565(0, 0, 0);
static const uint16_t WHITE = rgb565(255, 255, 255);
static const uint16_t GRAY  = rgb565(150, 150, 150);
static const uint16_t DIM   = rgb565(100, 100, 100);
static const uint16_t RED   = rgb565(230, 40, 40);
static const uint16_t BLUE  = rgb565(50, 120, 255);
static const uint16_t YELLOW = rgb565(255, 210, 40);
static const uint16_t BOARD = rgb565(20, 60, 170);
static const uint16_t FRAME = rgb565(45, 50, 62);   // screen-only board: neutral so red and blue pieces both stand out

static const int PW = 800, PH = 480;
// Finn's wide layout
static const int MENU_CX = 528, TITLE_CX = 208, LABEL_STEP = 34, ITEM_STEP = 48, BOX_H = 42, BOX_W = 300, GAP = 14;

struct MenuEntry { char kind; const char* text; int target; };   // kind: 'L' label, 'I' item, 'G' gap
// targets: 0..3 = game modes, 4 options, 5 stats
static const MenuEntry MENU[] = {
    {'L', "Player vs Player", -1}, {'I', "1v1", 0}, {'G', "", -1},
    {'L', "VS R.O.G.E.R", -1}, {'I', "Noob", 1}, {'I', "Pro", 2}, {'I', "Hacker", 3}, {'G', "", -1},
    {'I', "Options", 4}, {'I', "Statistics", 5},
};
static const int N_MENU = sizeof(MENU) / sizeof(MENU[0]);
static const int N_ITEMS = 6;
static const char* MODE_NAME[4] = { "1v1", "Noob", "Pro", "Hacker" };
static const char* MODE_BLURB[4] = { "Two players, pass and play", "R.O.G.E.R plays loose",
                                     "R.O.G.E.R plays sharp", "R.O.G.E.R plays perfect" };
static const int N_OPT = 6;   // Screen only toggle, Link debug, GIF test, Shut down, Servo calibration, Back
static const int N_FAULT = 4; // Back to menu, Restart gantry, Link debug, Options
static const int N_DBG = 6;   // Ping, Home, Column, Drop here, Restart, Back
static const uint16_t GREEN = rgb565(60, 200, 90);
static const uint16_t CYAN  = rgb565(90, 200, 230);

// What the hints call the select action: "Left+Right" when it is the two-arrow chord.
#define SEL (chordSelect ? std::string("Left+Right") : std::string("Select"))

static std::string fitText(std::string s, FontId f, int maxW) {
    if (Canvas::textWidth(s, f) <= maxW) return s;
    while (!s.empty() && Canvas::textWidth(s + "...", f) > maxW) s.pop_back();
    return s + "...";
}
static const char* DIFF_NAME[3] = { "Noob", "Pro", "Hacker" };

App::App(Brain& b, FacePlayer& f, Stats& s, std::string path, bool sim, SwitchLink* sw)
    : b_(b), faces_(f), stats_(s), statsPath_(std::move(path)), sim_(sim), sw_(sw) {
    showBoard = sim;
    sel_ = b_.menuIndex;          // remembered last mode = same index as the menu item
    screenInput_ = b_.screenInputSetting();
    lastState_ = b_.state;
}

// ---------------- input ----------------
bool App::espButton(uint8_t btn) {
    if (screen == Screen::GAME && b_.state == State::GAME_OVER) {       // results sequence owns the buttons
        gameOverInput(btn == BTN_LEFT ? A_UP : btn == BTN_RIGHT ? A_DOWN : A_SELECT);
        return true;
    }
    if (screen == Screen::GAME && b_.state != State::FAULT) {
        if (screenOnly() && screenInput_ == 0) { wrongInputAt_ = lastNow_; return true; }   // keyboard is playing
        return false;                                                                       // the game owns the buttons
    }
    handle(btn == BTN_LEFT ? A_UP : btn == BTN_RIGHT ? A_DOWN : A_SELECT);
    return true;
}

void App::key(Act a) {
    if (a == A_QUIT) { quit = true; return; }
    if (a == A_BOARD) { showBoard = !showBoard; return; }
    if (screen == Screen::GAME && b_.state == State::GAME_OVER) { gameOverInput(a); return; }
    if (screen == Screen::GAME && b_.state != State::FAULT) {
        if (screenOnly() && screenInput_ == 1 && (a == A_UP || a == A_DOWN || a == A_SELECT)) {
            wrongInputAt_ = lastNow_; return;                                               // the buttons are playing
        }
        if (a == A_UP) b_.onButton(BTN_LEFT);
        else if (a == A_DOWN) b_.onButton(BTN_RIGHT);
        else if (a == A_SELECT) b_.onButton(BTN_CONFIRM);
        return;
    }
    handle(a);
}

void App::handle(Act a) {
    switch (screen) {
    case Screen::MENU:
        if (a == A_UP) sel_ = (sel_ + N_ITEMS - 1) % N_ITEMS;
        else if (a == A_DOWN) sel_ = (sel_ + 1) % N_ITEMS;
        else if (a == A_SELECT) {
            if (sel_ <= 3) { confirmMode_ = sel_; confirmSel_ = 0; notice_.clear(); screen = Screen::CONFIRM; }
            else if (sel_ == 4) { optSel_ = 0; screen = Screen::OPTIONS; }
            else { confirmClear_ = false; screen = Screen::STATS; }
        }
        break;
    case Screen::CONFIRM:
        if (a == A_UP || a == A_DOWN) { confirmSel_ ^= 1; notice_.clear(); }
        else if (a == A_BACK) screen = Screen::MENU;
        else if (a == A_SELECT) {
            if (confirmSel_ == 1) { screen = Screen::MENU; break; }
            if (b_.state == State::MENU) { coinSel_ = 0; screen = Screen::COINCALL; }   // coin toss decides who goes first
            else notice_ = b_.state == State::HOMING ? "The gantry is still homing. Try again in a moment."
                                                     : "Can't start right now.";
        }
        break;
    case Screen::COINCALL:
        if (a == A_UP) coinSel_ = (coinSel_ + 2) % 3;
        else if (a == A_DOWN) coinSel_ = (coinSel_ + 1) % 3;
        else if (a == A_BACK || (a == A_SELECT && coinSel_ == 2)) screen = Screen::CONFIRM;
        else if (a == A_SELECT) {
            tossCall_ = coinSel_;
            tossResult_ = (int)(coinRng_() & 1);
            bool callerWins = tossCall_ == tossResult_;
            // vs R.O.G.E.R the human calls (human = BLACK = 1); in 1v1 Player One (RED = 0) calls
            tossFirst_ = confirmMode_ != 0 ? (callerWins ? 1 : 0) : (callerWins ? 0 : 1);
            tossStart_ = 0;
            screen = Screen::COINTOSS;
        }
        break;
    case Screen::COINTOSS:
        // the coin is in the air: nothing to do. Once it has landed, select starts right away.
        if (a == A_SELECT && tossStart_ && lastNow_ - tossStart_ >= (uint64_t)TOSS_MS) startTossedGame();
        break;
    case Screen::OPTIONS:
        if (a == A_UP) optSel_ = (optSel_ + N_OPT - 1) % N_OPT;
        else if (a == A_DOWN) optSel_ = (optSel_ + 1) % N_OPT;
        else if (a == A_BACK) screen = b_.state == State::FAULT ? Screen::GAME : Screen::MENU;
        else if (a == A_SELECT) {
            if (optSel_ == 0 && sw_) { smSel_ = screenOnly() ? screenInput_ : 1; screen = Screen::SCREENMODE; }   // default: buttons
            else if (optSel_ == 1) { dbgReturn_ = Screen::OPTIONS; openLinkDebug(); }
            else if (optSel_ == 2) { screen = Screen::GIFTEST; if (faces_.loaded()) faces_.playClip(0, 0); }
            else if (optSel_ == 3) { shutSel_ = 1; screen = Screen::SHUTDOWN; }
            else if (optSel_ == 4) openServoCal();
            else screen = b_.state == State::FAULT ? Screen::GAME : Screen::MENU;
        }
        break;
    case Screen::SCREENMODE: {
        int n = screenModeCount();
        if (a == A_UP) smSel_ = (smSel_ + n - 1) % n;
        else if (a == A_DOWN) smSel_ = (smSel_ + 1) % n;
        else if (a == A_BACK) screen = Screen::OPTIONS;
        else if (a == A_SELECT) {
            if (smSel_ <= 1) setScreenMode(true, (uint8_t)smSel_);          // Keyboard / Motion Controller buttons
            else if (screenOnly() && smSel_ == 2) setScreenMode(false, screenInput_);   // Turn off
            screen = Screen::OPTIONS;
        }
        break;
    }
    case Screen::SHUTDOWN:
        if (a == A_UP || a == A_DOWN) shutSel_ ^= 1;
        else if (a == A_BACK) screen = b_.state == State::FAULT ? Screen::GAME : Screen::OPTIONS;
        else if (a == A_SELECT) {
            if (shutSel_ == 0) shutdownRequested = true;
            else screen = b_.state == State::FAULT ? Screen::GAME : Screen::OPTIONS;
        }
        break;
    case Screen::STATS:
        if (a == A_CLEAR) {
            if (confirmClear_) { stats_.clear(); stats_.save(statsPath_); confirmClear_ = false; }
            else confirmClear_ = true;
            return;
        }
        confirmClear_ = false;
        if (a == A_SELECT || a == A_BACK) screen = Screen::MENU;
        else if (a == A_TEST) {          // Finn's random test game, for checking the layout
            static std::mt19937 r{std::random_device{}()};
            if (r() % 100 < 75) {
                int roll = r() % 100; const char* res = roll < 45 ? "win" : roll < 90 ? "loss" : "draw";
                stats_.recordRoger(r() % 3, res);
            } else {
                int roll = r() % 100; stats_.recordPvp(roll < 45 ? "p1" : roll < 90 ? "p2" : "draw");
            }
            stats_.save(statsPath_);
        }
        break;
    case Screen::GIFTEST:
        if (!faces_.loaded()) { if (a == A_SELECT || a == A_BACK) screen = Screen::OPTIONS; break; }
        if (a == A_UP) faces_.playClip(faces_.clipIndex - 1, 0);
        else if (a == A_DOWN) faces_.playClip(faces_.clipIndex + 1, 0);
        else if (a == A_SELECT || a == A_BACK) { faces_.play(faces_.idle, 0); screen = Screen::OPTIONS; }
        break;
    case Screen::GAME:   // only reached in FAULT: recover without power-cycling anything
        if (a == A_UP) faultSel_ = (faultSel_ + N_FAULT - 1) % N_FAULT;
        else if (a == A_DOWN) faultSel_ = (faultSel_ + 1) % N_FAULT;
        else if (a == A_SELECT || a == A_BACK) {
            int pick = a == A_BACK ? 0 : faultSel_;
            if (pick == 0) { b_.resetToMenu(); screen = Screen::MENU; sel_ = b_.menuIndex; }              // re-home, mode menu
            else if (pick == 1) { b_.restartMotionController(); screen = Screen::MENU; sel_ = b_.menuIndex; }
            else if (pick == 2) { dbgReturn_ = Screen::GAME; openLinkDebug(); }
            else { optSel_ = 0; screen = Screen::OPTIONS; }
            faultSel_ = 0;
        }
        break;
    case Screen::SERVOCAL:
        servoCalInput(a);
        break;
    case Screen::LINKDEBUG:
        if (a == A_UP) dbgSel_ = (dbgSel_ + N_DBG - 1) % N_DBG;
        else if (a == A_DOWN) dbgSel_ = (dbgSel_ + 1) % N_DBG;
        else if (a == A_BACK || (a == A_SELECT && dbgSel_ == 5))
            screen = b_.state == State::FAULT ? Screen::GAME : dbgReturn_ == Screen::GAME ? Screen::MENU : dbgReturn_;
        else if (a == A_SELECT) {
            if (dbgSel_ == 0) { b_.debugPing(); dbgLastPing_ = lastNow_; }
            else if (dbgSel_ == 1) { if (b_.state != State::HOMING) b_.resetToMenu(); }
            else if (dbgSel_ == 2) { dbgCol_ = (dbgCol_ + 1) % 7; b_.debugMove(dbgCol_); }
            else if (dbgSel_ == 3) b_.debugDrop(dbgCol_, 0);
            else if (dbgSel_ == 4) b_.restartMotionController();
        }
        break;
    }
}

void App::setScreenMode(bool on, uint8_t input) {
    if (!sw_) return;
    if (b_.state != State::MENU && b_.state != State::HOMING && b_.state != State::FAULT) return;   // never mid-game
    screenInput_ = input ? 1 : 0;
    bool changed = (sw_->screenOnly != on);
    sw_->screenOnly = on;
    b_.saveScreenMode(on, screenInput_);
    if (changed) b_.rehome();     // the new "gantry" (real or on-screen) homes and reports READY
}

// ---------------- brain -> screen ----------------
std::string App::groupFor(FaceEvent f) const {
    static const std::vector<std::string> GOOD = {"good_move", "good"}, BAD = {"bad_move", "bad"},
        WIN = {"roger_win", "roger_wins", "win", "wins", "victory"},
        LOSS = {"roger_loss", "roger_lose", "roger_loses", "loss", "lose", "defeat"}, THINK = {"thinking", "think"};
    const std::vector<std::string>* c = nullptr;
    switch (f) {
    case FACE_GOOD_MOVE: c = &GOOD; break;
    case FACE_BAD_MOVE:  c = &BAD; break;
    case FACE_WIN:       c = &WIN; break;
    case FACE_LOSS:      c = &LOSS; break;
    case FACE_THINKING:  c = &THINK; break;
    default: return faces_.idle;
    }
    for (const auto& g : *c) if (faces_.has(g)) return g;
    return faces_.idle;           // no clip for this event yet: stay on idle
}

bool App::isReaction(const std::string& g) const {
    return !g.empty() && (g == groupFor(FACE_GOOD_MOVE) || g == groupFor(FACE_BAD_MOVE)) && g != faces_.idle;
}

// Every face loops. A reaction (good/bad move) starts the moment the human
// drops and keeps looping through R.O.G.E.R's turn; when something else is
// asked for, it finishes the pass it is on first, so it is never cut off.
void App::applyFace(FaceEvent f) {
    if (!faces_.loaded()) return;
    if (f == FACE_WIN || f == FACE_LOSS) return;      // the game-over sequence plays these (once, then the popup)
    std::string target = f == FACE_IDLE ? faces_.idle : groupFor(f);
    if (isReaction(faces_.current)) {
        if (target == faces_.current) faces_.keepLooping();
        else faces_.finishPassThen(target);
    } else if (target != faces_.current || faces_.oneShotPlaying()) {
        faces_.play(target, 0);
    }
    faceDirty_ = true;
}

void App::recordResult() {
    if (b_.mode == 0) stats_.recordPvp(b_.winner == 0 ? "p1" : b_.winner == 1 ? "p2" : "draw");
    else stats_.recordRoger(b_.mode - 1, b_.winner == 0 ? "win" : b_.winner == 1 ? "loss" : "draw");  // Roger is RED
    stats_.save(statsPath_);
}

void App::syncWithBrain(uint64_t now) {
    State s = b_.state;
    if (s != lastState_) {
        if (s == State::AWAITING_DROP) { dropStart_ = now; dropCol_ = b_.pendingCol(); dropColor_ = b_.board.toMove(); }
        if (s == State::GAME_OVER) { recordResult(); startGameOver(now); }
        else over_ = Over::NONE;
        if (screen == Screen::GAME && (s == State::HOMING || s == State::MENU)) {
            screen = Screen::MENU;               // game over -> re-home -> back to the menu
            sel_ = b_.menuIndex;
        }
        if (s == State::FAULT && screen != Screen::SHUTDOWN && screen != Screen::LINKDEBUG) { screen = Screen::GAME; faultSel_ = 0; }
        lastState_ = s;
    }
    if (b_.shutdownRequested) shutdownRequested = true;         // shutdown button on the Motion Controller
    if (b_.faceSeq != lastFaceSeq_) {
        lastFaceSeq_ = b_.faceSeq;
        if (screen == Screen::GAME) applyFace(b_.face);
    }
}

std::string App::statusText(uint64_t now) const {
    bool ai = b_.mode != 0;
    switch (b_.state) {
    case State::HUMAN_TURN:
        if (ai) return "Your turn";
        return b_.board.toMove() == 0 ? "Player One's turn" : "Player Two's turn";
    case State::AI_TURN: return std::string("R.O.G.E.R is thinking") + std::string(1 + (now / 400) % 3, '.');
    case State::AWAITING_DROP: return "Dropping...";
    default: return "";
    }
}

std::string App::signature(uint64_t now) const {
    char buf[256];
    const C4State& st = b_.board.state();
    snprintf(buf, sizeof buf, "%d|%d|%d%d|%d|%d|%d|%d|%llx|%llx|%d|%d|%d|%d|%d|%d|%u|%u|%u|%u|%u",
             (int)screen, sel_, confirmMode_, confirmSel_, optSel_, shutSel_, confirmClear_, (int)b_.state,
             (unsigned long long)st.bitboard[0], (unsigned long long)st.bitboard[1], b_.selectedCol, b_.winner,
             b_.faultCode, showBoard, faces_.clipIndex, shutdownRequested,
             (unsigned)stats_.totals().games(), (unsigned)stats_.pvpGames, (unsigned)notice_.size(),
             (unsigned)b_.faceSeq, (unsigned)(stats_.totals().wins * 7 + stats_.p1 * 3 + stats_.p2));
    std::string extra = screenOnly() ? (screenInput_ ? "|sob" : "|sok") : "|g";
    extra += (now < bannerUntil_ ? "|B" : "|-");
    extra += "|c" + std::to_string(coinSel_) + "|o" + std::to_string((int)over_) + "|f" + std::to_string(faultSel_);
    if (screen == Screen::COINTOSS) extra += "|t" + std::to_string(tossStart_ ? (now - tossStart_ < (uint64_t)TOSS_MS ? (now - tossStart_) / 16 : 9999) : 0);
    if (screen == Screen::MENU && b_.state == State::HOMING)
        extra += "|h" + std::to_string((now - b_.stateSince()) / 1000) + (b_.boardSilent() ? "S" : "") + std::to_string(b_.linkLog.size());
    if (screen == Screen::LINKDEBUG) {     // live screen: redraw on traffic and every half second
        const LinkCounters& lc = b_.linkCounters();
        extra += "|d" + std::to_string(dbgSel_) + "," + std::to_string(dbgCol_) + "," + std::to_string(b_.linkLog.size()) + "," +
                 std::to_string(lc.rxBytes) + "," + std::to_string(lc.txFrames) + "," + std::to_string(now / 500) +
                 (b_.linkLog.empty() ? "" : "," + std::to_string(b_.linkLog.back().t));
    }
    if (screen == Screen::SERVOCAL) {
        extra += "|s" + std::to_string(calSel_) + (calEditing_ ? "e" : "n");
        for (int v : calVals_) extra += "," + std::to_string(v);
        if (mc_) { auto st = mc_->status(); extra += "," + std::to_string(st.saveResult) + (st.busy ? "b" : "i") + (st.homed ? "h" : "u"); }
    }
    extra += "|" + std::to_string(smSel_) + (wrongInputAt_ && now - wrongInputAt_ < 2500 ? "|w" : "|");
    if (screenOnly() && b_.state == State::AWAITING_DROP) extra += std::to_string((now - dropStart_) / 16);   // ~60 fps fall
    return std::string(buf) + statusText(now) + extra;
}

bool App::frame(uint64_t now, Canvas& cv) {
    lastNow_ = now;
    syncWithBrain(now);
    bool faceOnScreen = (screen == Screen::GAME && b_.mode != 0 && b_.state != State::FAULT) || screen == Screen::GIFTEST;
    bool changed = false;
    // Keep the face clock running even while no face is on screen, so a clip
    // started later is timed from now (otherwise it would fast-forward).
    if (faces_.loaded()) changed = faces_.update(now) && faceOnScreen;
    if (over_ != Over::NONE) advanceGameOver(now);
    if (screen == Screen::LINKDEBUG && now - dbgLastPing_ >= 3000 && b_.debugAllowed()) { b_.debugPing(); dbgLastPing_ = now; }
    if (screen == Screen::COINTOSS) {
        if (!tossStart_) tossStart_ = now;
        if (now - tossStart_ >= (uint64_t)(TOSS_MS + TOSS_HOLD_MS)) startTossedGame();
    }
    std::string sig = signature(now);
    if (sig != lastSig_ || changed || faceDirty_) {
        lastSig_ = sig; faceDirty_ = false;
        draw(cv, now);
        return true;
    }
    return false;
}

// ---------------- drawing ----------------
void App::hint(Canvas& cv, const std::string& s) { cv.textCentered(s, F_SMALL, DIM, PW / 2, PH - 40); }

void App::drawItem(Canvas& cv, int y, const std::string& label, bool selected, int cx, int boxW) {
    uint16_t color = WHITE;
    if (selected) { cv.roundRect(cx - boxW / 2, y, boxW, BOX_H, 10, WHITE); color = BLACK; }
    int th = Canvas::fontHeight(F_ITEM);
    cv.textCentered(label, F_ITEM, color, cx, y + (BOX_H - th) / 2);
}

void App::draw(Canvas& cv, uint64_t now) {
    cv.fill(BLACK);
    switch (screen) {
    case Screen::MENU:     drawMenu(cv, now); break;
    case Screen::CONFIRM:  drawConfirm(cv); break;
    case Screen::COINCALL: drawCoinCall(cv); break;
    case Screen::COINTOSS: drawCoinToss(cv, now); break;
    case Screen::OPTIONS:  drawOptions(cv); break;
    case Screen::SCREENMODE: drawScreenMode(cv); break;
    case Screen::SHUTDOWN: drawShutdown(cv); break;
    case Screen::STATS:    drawStats(cv); break;
    case Screen::GIFTEST:  drawGifTest(cv); break;
    case Screen::GAME:     drawGame(cv, now); break;
    case Screen::LINKDEBUG: drawLinkDebug(cv, now); break;
    case Screen::SERVOCAL: drawServoCal(cv); break;
    }
    if (now < bannerUntil_) {
        int bw = PW - 60, bh = 96, bx = 30, by = PH / 2 - bh / 2;
        cv.roundRect(bx, by, bw, bh, 12, rgb565(150, 0, 0));
        cv.roundRectOutline(bx, by, bw, bh, 12, 2, WHITE);
        cv.textCentered(fitText(banner1_, F_SMALL, bw - 30), F_SMALL, WHITE, PW / 2, by + 20);
        cv.textCentered(fitText(banner2_, F_SMALL, bw - 30), F_SMALL, rgb565(255, 210, 200), PW / 2, by + 56);
    }
}

void App::drawMenu(Canvas& cv, uint64_t now) {
    int th = Canvas::fontHeight(F_TITLE);
    int ty = (PH - 40 - th) / 2;
    cv.textCentered("CONNECT 4", F_TITLE, WHITE, TITLE_CX, ty);
    // gantry status (not in Finn's sketch: the game can't start until homing is done)
    char hb[48];
    snprintf(hb, sizeof hb, "gantry homing... %ds", (int)((now - b_.stateSince()) / 1000));
    const char* g = screenOnly() ? (screenInput_ ? "screen only - Motion Controller buttons" : "screen only - keyboard") :
                    b_.state == State::HOMING ? hb : b_.state == State::FAULT ? "gantry fault" : "gantry ready";
    cv.textCentered(g, F_SMALL, screenOnly() ? YELLOW : b_.state == State::MENU ? GRAY : DIM, TITLE_CX, ty + th + 14);
    if (!screenOnly() && b_.boardSilent()) {
        cv.textCentered("NO SIGNAL from the", F_SMALL, RED, TITLE_CX, ty + th + 42);
        cv.textCentered("Motion Controller", F_SMALL, RED, TITLE_CX, ty + th + 64);
        cv.textCentered("Its ESP32 has not started.", F_SMALL, GRAY, TITLE_CX, ty + th + 94);
        cv.textCentered("Press its EN button.", F_SMALL, GRAY, TITLE_CX, ty + th + 116);
    } else if (!screenOnly() && b_.state == State::HOMING && b_.mc.lastFrame && now - b_.stateSince() > 4000) {
        cv.textCentered("Motion Controller is running:", F_SMALL, DIM, TITLE_CX, ty + th + 42);
        cv.textCentered(fitText(b_.lastBoardEvent(), F_SMALL, 2 * TITLE_CX - 20), F_SMALL, GRAY, TITLE_CX, ty + th + 64);
    }

    int h = 0;
    for (const auto& e : MENU) h += e.kind == 'G' ? GAP : e.kind == 'L' ? LABEL_STEP : ITEM_STEP;
    int y = std::max(10, (PH - 40 - h) / 2);
    int item = 0;
    for (const auto& e : MENU) {
        if (e.kind == 'G') y += GAP;
        else if (e.kind == 'L') { cv.textCentered(e.text, F_LABEL, GRAY, MENU_CX, y); y += LABEL_STEP; }
        else { drawItem(cv, y, e.text, item == sel_, MENU_CX); y += ITEM_STEP; item++; }
    }
    hint(cv, "Left/Right: move    " + SEL + ": choose");
    cv.textRight("build " C4_BUILD, F_SMALL, rgb565(70, 70, 70), PW - 10, PH - 26);
}

void App::drawConfirm(Canvas& cv) {
    int cx = PW / 2;
    cv.textCentered(confirmMode_ == 0 ? "Player vs Player" : "VS R.O.G.E.R", F_LABEL, GRAY, cx, 70);
    cv.textCentered(MODE_NAME[confirmMode_], F_BIG, WHITE, cx, 110);
    cv.textCentered(MODE_BLURB[confirmMode_], F_LABEL, GRAY, cx, 180);
    drawItem(cv, 250, "Start game", confirmSel_ == 0, cx);
    drawItem(cv, 250 + ITEM_STEP, "Back", confirmSel_ == 1, cx);
    if (!notice_.empty()) cv.textCentered(notice_, F_SMALL, RED, cx, 370);
    else if (b_.state == State::HOMING) cv.textCentered("Waiting for the gantry to finish homing...", F_SMALL, DIM, cx, 370);
    hint(cv, "Left/Right: move    " + SEL + ": choose");
}

void App::drawOptions(Canvas& cv) {
    int cx = PW / 2;
    cv.textCentered("Options", F_BIG, WHITE, cx, 30);
    int y = 96;
    const char* items[N_OPT] = { !screenOnly() ? "Screen only: OFF" : screenInput_ ? "Screen only: Buttons" : "Screen only: Keyboard",
                                 "Link debug", "GIF test", "Shut down", "Servo calibration", "Back" };
    for (int i = 0; i < N_OPT; i++) { drawItem(cv, y, items[i], i == optSel_, cx, 380); y += ITEM_STEP; }
    if (!sw_) cv.textCentered("(screen only is not available in this build)", F_SMALL, DIM, cx, y + 16);
    else if (screenOnly()) cv.textCentered(screenInput_ ? "Games play on the display with the Motion Controller buttons. Gantry parked."
                                                        : "Games play on the display with the Pi's keyboard. Gantry parked.", F_SMALL, GRAY, cx, y + 16);
    else cv.textCentered("Turn on to play a whole game on the display, without the gantry.", F_SMALL, DIM, cx, y + 16);
    hint(cv, "Left/Right: move    " + SEL + ": choose");
}

void App::drawShutdown(Canvas& cv) {
    int cx = PW / 2, top = 70;
    cv.textCentered("Shut down?", F_BIG, WHITE, cx, top + 40);
    cv.textCentered("Always shut down before unplugging,", F_LABEL, GRAY, cx, top + 130);
    cv.textCentered("or the SD card can get corrupted.", F_LABEL, GRAY, cx, top + 165);
    drawItem(cv, top + 220, "Shut down", shutSel_ == 0, cx);
    drawItem(cv, top + 220 + ITEM_STEP, "Cancel", shutSel_ == 1, cx);
    if (!powerOffAllowed) cv.textCentered("Power-off permission missing: run install_autostart.sh again.", F_SMALL, YELLOW, cx, PH - 70);
    hint(cv, "Left/Right: move    " + SEL + ": choose");
}

void App::drawShuttingDown(Canvas& cv) {
    int cx = PW / 2, top = 110;
    cv.fill(BLACK);
    cv.textCentered("Shutting down...", F_BIG, WHITE, cx, top + 40);
    cv.textCentered("Wait for the Pi's green light to stop", F_LABEL, GRAY, cx, top + 130);
    cv.textCentered("blinking before unplugging.", F_LABEL, GRAY, cx, top + 165);
}

void App::statRow(Canvas& cv, int y, const std::string& label, const WLD& r) {
    const int lx = 30, wx = 185, lsx = 230, dx = 275, rr = PW / 2 - 20;
    cv.text(label, F_TABLE, WHITE, lx, y);
    cv.textCentered(std::to_string(r.wins), F_TABLE, WHITE, wx, y);
    cv.textCentered(std::to_string(r.losses), F_TABLE, WHITE, lsx, y);
    cv.textCentered(std::to_string(r.draws), F_TABLE, WHITE, dx, y);
    char rate[16];
    if (r.games()) snprintf(rate, sizeof rate, "%.0f%%", 100.0 * r.wins / r.games()); else snprintf(rate, sizeof rate, "-");
    cv.textRight(rate, F_TABLE, WHITE, rr, y);
}

void App::drawStats(Canvas& cv) {
    int cx = PW / 2;
    const int wx = 185, lsx = 230, dx = 275, rr = PW / 2 - 20;
    cv.textCentered("Statistics", F_BIG, WHITE, cx, 20);
    int y = 100;
    cv.textCentered("R.O.G.E.R record", F_LABEL, GRAY, PW / 4, y);
    y += 45;
    cv.textCentered("W", F_TABLE, GRAY, wx, y); cv.textCentered("L", F_TABLE, GRAY, lsx, y);
    cv.textCentered("D", F_TABLE, GRAY, dx, y); cv.textRight("Win %", F_TABLE, GRAY, rr, y);
    y += 40;
    for (int d = 0; d < 3; d++) { statRow(cv, y, DIFF_NAME[d], stats_.roger[d]); y += 42; }
    cv.hline(25, rr, y + 2, DIM);
    y += 12;
    WLD t = stats_.totals();
    statRow(cv, y, "Overall", t);

    for (int yy = 100; yy < PH - 70; yy++) cv.px[(size_t)yy * PW + cx] = DIM;   // divider
    int left = cx + 30, right = PW - 30;
    y = 100;
    cv.textCentered("Player vs Player", F_LABEL, GRAY, PW * 3 / 4, y);
    y += 45;
    const std::pair<const char*, int> rows[] = { {"Games", stats_.pvpGames}, {"Player One wins", stats_.p1},
                                                 {"Player Two wins", stats_.p2}, {"Draws", stats_.pvpDraws} };
    for (const auto& r : rows) {
        cv.text(r.first, F_TABLE, WHITE, left, y); cv.textRight(std::to_string(r.second), F_TABLE, WHITE, right, y); y += 40;
    }
    y += 10;
    cv.text("Total games played", F_TABLE, WHITE, left, y);
    cv.textRight(std::to_string(t.games() + stats_.pvpGames), F_TABLE, WHITE, right, y);
    if (confirmClear_) cv.textCentered("Press C again to clear all stats", F_SMALL, RED, cx, PH - 75);
    hint(cv, SEL + ": back    (keyboard: T test game, C clear)");
}

void App::drawGifTest(Canvas& cv) {
    if (!faces_.loaded()) {
        cv.textCentered("GIF test", F_BIG, WHITE, PW / 2, 100);
        cv.textCentered("No faces loaded.", F_LABEL, GRAY, PW / 2, 200);
        cv.textCentered("Run convert.bat on the laptop and copy the faces", F_SMALL, DIM, PW / 2, 250);
        cv.textCentered("folder onto the SD card (see README).", F_SMALL, DIM, PW / 2, 275);
        hint(cv, SEL + ": back");
        return;
    }
    cv.blitScaled(faces_.frame(), faces_.width, faces_.height, faces_.bakedRotate == 180);
    char buf[160];
    snprintf(buf, sizeof buf, "%d/%zu  %s  (%s)", faces_.clipIndex + 1, faces_.clips.size(),
             faces_.clips[faces_.clipIndex].name.c_str(), faces_.current.c_str());
    std::string gifHint = "Left/Right: prev/next   " + SEL + ": back";
    int w1 = Canvas::textWidth(buf, F_SMALL), w2 = Canvas::textWidth(gifHint, F_SMALL);
    cv.rectAlpha(6, 6, std::max(w1, w2) + 16, 52, BLACK, 190);
    cv.text(buf, F_SMALL, WHITE, 14, 10);
    cv.text(gifHint, F_SMALL, GRAY, 14, 32);
}

void App::drawSelector(Canvas& cv) {
    if (b_.state != State::HUMAN_TURN) return;
    cv.rectAlpha(0, PH - 80, PW, 80, BLACK, 150);
    int step = PW / 7;
    for (int c = 0; c < 7; c++) {
        int x = step / 2 + c * step, y = PH - 40;
        if (c == b_.selectedCol) { cv.circle(x, y, 24, WHITE); cv.circle(x, y, 20, b_.board.toMove() == 0 ? RED : BLUE); }
        else cv.circle(x, y, 12, b_.board.legal(c) ? GRAY : rgb565(45, 45, 45));
    }
}

void App::drawMiniBoard(Canvas& cv) {
    // top-right over R.O.G.E.R's face; on the 1v1 screen, lower right so it clears the turn text
    const int cell = 26, bw = cell * 7, bh = cell * 6, x0 = PW - bw - 12;
    const int y0 = b_.mode == 0 ? PH - 80 - bh - 18 : 12;
    cv.roundRect(x0 - 6, y0 - 6, bw + 12, bh + 12, 8, BOARD);
    for (int c = 0; c < 7; c++) for (int r = 0; r < 6; r++) {
        int v = b_.board.at(c, r);
        cv.circle(x0 + c * cell + cell / 2, y0 + (5 - r) * cell + cell / 2, 10, v == 0 ? RED : v == 1 ? BLUE : BLACK);
    }
}

void App::drawFault(Canvas& cv) {
    cv.fill(rgb565(150, 0, 0));
    cv.textCentered("MOTION FAULT", F_BIG, WHITE, PW / 2, 22);
    char buf[64]; snprintf(buf, sizeof buf, "code 0x%02X", b_.faultCode);
    cv.textCentered(buf, F_LABEL, WHITE, PW / 2, 98);
    uint8_t c = b_.faultCode;
    const char* why = c == 0xFF ? "The boards are out of sync (wrong column confirmed)." :
                      c == 0xFD ? "No reply after a drop. Check whether the piece fell." :
                      c == 0xFC ? "The Motion Controller can't hear the Pi." :
                      c == 0xFB ? "The Motion Controller restarted during the game." :
                      c == 0x01 ? "The gantry stalled or could not find home." :
                      c == 0x04 ? "A drop was requested before homing." : "The Motion Controller reported an error.";
    const char* tip = c == 0xFD ? "Link debug shows whether the board heard the drop." :
                      c == 0xFC ? "Check the wire from Pi pin 8 to Motion Controller GPIO14." :
                      c == 0xFB ? (b_.mc.resetReason == 9 ? "Reason: brownout. The supply dipped when the motor or servo started."
                                                          : "Link debug shows the restart reason.") :
                      c == 0x01 ? "Check the limit switch and that the motor turns." : "Link debug shows what the board reported.";
    cv.textCentered(why, F_SMALL, WHITE, PW / 2, 140);
    cv.textCentered(tip, F_SMALL, rgb565(255, 210, 200), PW / 2, 168);
    static const char* items[N_FAULT] = { "Back to menu", "Restart gantry", "Link debug", "Options" };
    static const char* help[N_FAULT] = { "Re-homes the gantry and goes back to mode selection.",
                                         "Reboots the Motion Controller (same as its EN button).",
                                         "Live traffic between the Pi and the Motion Controller.",
                                         "Screen only, GIF test, shut down." };
    int y = 210;
    for (int i = 0; i < N_FAULT; i++) { drawItem(cv, y, items[i], i == faultSel_, PW / 2, 400); y += ITEM_STEP; }
    cv.textCentered(help[faultSel_], F_SMALL, rgb565(255, 200, 190), PW / 2, PH - 40);
}

void App::drawPvp(Canvas& cv) {
    bool over = b_.state == State::GAME_OVER;
    int player = b_.board.toMove();
    if (over) {
        const char* t = b_.winner == 2 ? "Draw" : b_.winner == 0 ? "Player One wins!" : "Player Two wins!";
        uint16_t c = b_.winner == 0 ? RED : b_.winner == 1 ? BLUE : WHITE;
        cv.circle(PW / 4 + 20, PH / 2 - 20, 110, c);
        cv.textCentered(t, F_BIG, c, PW * 5 / 8 + 20, PH / 2 - 75);
        return;
    }
    uint16_t color = player == 0 ? RED : BLUE;
    cv.circle(PW / 4 + 20, PH / 2 - 60, 110, color);
    int tx = PW * 5 / 8 + 20;
    cv.textCentered(player == 0 ? "Player One's" : "Player Two's", F_BIG, color, tx, PH / 2 - 115);
    cv.textCentered("turn", F_BIG, color, tx, PH / 2 - 45);
    if (b_.state == State::AWAITING_DROP) cv.textCentered("Dropping...", F_LABEL, GRAY, tx, PH / 2 + 30);
    drawSelector(cv);
}

void App::drawGame(Canvas& cv, uint64_t now) {
    if (b_.state == State::FAULT) { drawFault(cv); return; }
    if (b_.state == State::GAME_OVER && over_ == Over::POPUP) { drawGameOverPopup(cv); return; }
    if (screenOnly()) { drawScreenOnly(cv, now); return; }
    if (b_.mode == 0) { drawPvp(cv); if (showBoard) drawMiniBoard(cv); return; }

    if (faces_.loaded()) cv.blitScaled(faces_.frame(), faces_.width, faces_.height, faces_.bakedRotate == 180);
    else {
        cv.textCentered("R.O.G.E.R", F_TITLE, WHITE, PW / 2, 120);
        cv.textCentered("(no face files loaded, see Options > GIF test)", F_SMALL, DIM, PW / 2, 200);
    }

    if (b_.state == State::GAME_OVER) {
        const char* t = b_.winner == 2 ? "Draw" : b_.winner == 0 ? "R.O.G.E.R wins" : "You win";
        int w = Canvas::textWidth(t, F_LABEL);
        cv.rectAlpha(10, 10, w + 24, 40, BLACK, 170);
        cv.text(t, F_LABEL, b_.winner == 0 ? RED : b_.winner == 1 ? YELLOW : WHITE, 22, 18);
    } else {
        std::string s = statusText(now);
        if (!s.empty()) {
            int w = Canvas::textWidth(s, F_LABEL);
            cv.rectAlpha(10, 10, w + 24, 40, BLACK, 170);
            cv.text(s, F_LABEL, b_.state == State::HUMAN_TURN ? YELLOW : WHITE, 22, 18);
        }
        drawSelector(cv);
    }
    if (showBoard) drawMiniBoard(cv);
}

// ---------------- screen-only play ----------------
// Layout: the board on the left half (with the hover piece above it and the
// falling-piece animation), R.O.G.E.R's face at its native 400x240 on the right.
static const int CELL = 52, BX0 = 10, BY0 = 118, BPAD = 10;
static int colX(int c) { return BX0 + BPAD + c * CELL + CELL / 2; }
static int rowY(int r) { return BY0 + BPAD + (5 - r) * CELL + CELL / 2; }
static const int HOVER_Y = BY0 - CELL / 2 - 4, PIECE_R = 22;

// Cells (bit = col*7+row) that are part of a four-in-a-row for `player`.
static uint64_t winningCells(const Board& b, int player) {
    uint64_t out = 0;
    const int dirs[4][2] = { {1, 0}, {0, 1}, {1, 1}, {1, -1} };
    for (int c = 0; c < 7; c++) for (int r = 0; r < 6; r++) for (auto& d : dirs) {
        int ec = c + 3 * d[0], er = r + 3 * d[1];
        if (ec < 0 || ec > 6 || er < 0 || er > 5) continue;
        bool all = true;
        for (int k = 0; k < 4 && all; k++) all = b.at(c + k * d[0], r + k * d[1]) == player;
        if (all) for (int k = 0; k < 4; k++) out |= 1ULL << ((c + k * d[0]) * 7 + r + k * d[1]);
    }
    return out;
}

void App::drawBigBoard(Canvas& cv, uint64_t now) {
    auto colorOf = [](int p) { return p == 0 ? RED : BLUE; };
    // hover piece over the selected column on a human turn
    if (b_.state == State::HUMAN_TURN) {
        int x = colX(b_.selectedCol);
        cv.circle(x, HOVER_Y, PIECE_R, colorOf(b_.board.toMove()));
        cv.circle(x, HOVER_Y, 6, WHITE);
    }
    // falling piece (drawn before the frame so the frame's holes show it sliding down)
    bool falling = b_.state == State::AWAITING_DROP && dropCol_ >= 0 && dropCol_ < 7;
    cv.roundRect(BX0, BY0, 7 * CELL + 2 * BPAD, 6 * CELL + 2 * BPAD, 14, FRAME);
    uint64_t win = b_.state == State::GAME_OVER && b_.winner <= 1 ? winningCells(b_.board, b_.winner) : 0;
    for (int c = 0; c < 7; c++) for (int r = 0; r < 6; r++) {
        int v = b_.board.at(c, r);
        int x = colX(c), y = rowY(r);
        cv.circle(x, y, PIECE_R, v < 0 ? BLACK : colorOf(v));
        if (win & (1ULL << (c * 7 + r))) { cv.circle(x, y, 9, WHITE); }
    }
    if (falling) {
        int row = b_.board.state().heights[dropCol_];
        if (row < 6) {
            double t = std::min(1.0, (double)(now - dropStart_) / 350.0);
            int y = HOVER_Y + (int)((rowY(row) - HOVER_Y) * t * t);     // accelerates like gravity
            cv.circle(colX(dropCol_), y, PIECE_R, colorOf(dropColor_));
        }
    }
}

void App::drawScreenOnly(Canvas& cv, uint64_t now) {
    const int RX = 400, RCX = 600;      // right panel
    bool ai = b_.mode != 0, over = b_.state == State::GAME_OVER;
    drawBigBoard(cv, now);

    // top-left status
    // (1v1 already shows whose turn it is on the right)
    std::string s = over || (!ai && b_.state == State::HUMAN_TURN) ? "" : statusText(now);
    if (!s.empty()) cv.text(s, F_LABEL, b_.state == State::HUMAN_TURN ? YELLOW : WHITE, BX0 + 4, 16);

    if (ai) {
        if (faces_.loaded()) {
            cv.blit(faces_.frame(), faces_.width, faces_.height, RX, 24, 400, 240, faces_.bakedRotate == 180);   // any converted size fits the panel
        } else {
            cv.textCentered("R.O.G.E.R", F_BIG, WHITE, RCX, 110);
            cv.textCentered("(no face files loaded)", F_SMALL, DIM, RCX, 180);
        }
        char m[64]; snprintf(m, sizeof m, "VS R.O.G.E.R  -  %s", MODE_NAME[b_.mode]);
        cv.textCentered(m, F_LABEL, GRAY, RCX, 280);
        if (over) {
            const char* t = b_.winner == 2 ? "DRAW" : b_.winner == 0 ? "R.O.G.E.R WINS" : "YOU WIN!";
            cv.textCentered(t, F_ITEM, b_.winner == 0 ? RED : b_.winner == 1 ? YELLOW : WHITE, RCX, 330);
        }
    } else {
        if (over) {
            const char* t = b_.winner == 2 ? "Draw" : b_.winner == 0 ? "Player One wins!" : "Player Two wins!";
            uint16_t c = b_.winner == 0 ? RED : b_.winner == 1 ? BLUE : WHITE;
            cv.circle(RCX, 150, 70, c);
            cv.textCentered(t, F_ITEM, c, RCX, 250);
        } else {
            int p = b_.board.toMove();
            uint16_t c = p == 0 ? RED : BLUE;
            cv.circle(RCX, 150, 70, c);
            cv.textCentered(p == 0 ? "Player One's" : "Player Two's", F_ITEM, c, RCX, 240);
            cv.textCentered("turn", F_ITEM, c, RCX, 285);
        }
        cv.textCentered("1v1  -  screen only", F_LABEL, GRAY, RCX, 40);
    }
    if (!over) cv.textCentered(screenInput_ ? (chordSelect ? "Buttons: left/right = column, both together = drop" : "Buttons: left/right = column, select = drop")
                                            : "Keyboard: arrows = column, Enter = drop", F_SMALL, DIM, RCX, PH - 40);
    if (wrongInputAt_ && now - wrongInputAt_ < 2500) {
        const char* m = screenInput_ ? "Use the Motion Controller buttons" : "Use the keyboard";
        int w = Canvas::textWidth(m, F_SMALL) + 24;
        cv.roundRect(RCX - w / 2, PH - 82, w, 30, 8, rgb565(120, 20, 20));
        cv.textCentered(m, F_SMALL, WHITE, RCX, PH - 77);
    }
}

void App::drawScreenMode(Canvas& cv) {
    int cx = PW / 2;
    cv.textCentered("Screen only", F_BIG, WHITE, cx, 24);
    cv.textCentered("Play a whole game on the display. The gantry stays parked.", F_SMALL, GRAY, cx, 96);
    cv.textCentered("Control the game with:", F_LABEL, GRAY, cx, 130);
    const char* on[4]  = { "Keyboard", "Motion Controller buttons", "Turn off screen only", "Back" };
    const char* off[3] = { "Keyboard", "Motion Controller buttons", "Back" };
    int n = screenModeCount(), y = 172;
    for (int i = 0; i < n; i++) {
        drawItem(cv, y, screenOnly() ? on[i] : off[i], i == smSel_, cx, 560);
        if (screenOnly() && i == screenInput_) cv.text("current", F_SMALL, i == smSel_ ? GRAY : YELLOW, cx + 290, y + 12);
        y += ITEM_STEP + (i == 1 ? 12 : 0);
    }
    cv.textCentered("Menus always work with both.", F_SMALL, DIM, cx, y + 10);
    hint(cv, "Left/Right: move    " + SEL + ": choose");
}

// ---------------- coin toss ----------------
void App::startTossedGame() {
    if (b_.startGame(confirmMode_, tossFirst_)) {
        screen = Screen::GAME;
        lastState_ = b_.state;
        lastFaceSeq_ = b_.faceSeq;
        faceDirty_ = true;
        if (faces_.loaded()) faces_.play(b_.face == FACE_THINKING ? groupFor(FACE_THINKING) : faces_.idle, 0);
    } else {
        notice_ = b_.state == State::HOMING ? "The gantry is still homing. Try again in a moment." : "Can't start right now.";
        screen = Screen::CONFIRM;
    }
}

void App::drawCoinCall(Canvas& cv) {
    int cx = PW / 2;
    cv.textCentered("Coin toss", F_BIG, WHITE, cx, 30);
    cv.textCentered(confirmMode_ == 0 ? "Player One, call it. The winner goes first." : "Call it. The winner goes first.",
                    F_LABEL, GRAY, cx, 110);
    const char* items[3] = { "Heads", "Tails", "Back" };
    int y = 180;
    for (int i = 0; i < 3; i++) { drawItem(cv, y, items[i], i == coinSel_, cx); y += ITEM_STEP + (i == 1 ? 14 : 0); }
    hint(cv, "Left/Right: move    " + SEL + ": choose");
}

// angle 0 = heads facing the screen, PI = tails; the coin spins about its vertical axis.
void App::drawCoin(Canvas& cv, int cx, int cy, int r, double angle) {
    double c = std::cos(angle);
    int rx = (int)(r * std::fabs(c));
    bool heads = c >= 0;
    uint16_t rim = heads ? rgb565(170, 120, 20) : rgb565(110, 110, 120);
    uint16_t face = heads ? rgb565(240, 190, 50) : rgb565(195, 195, 205);
    cv.ellipse(cx, cy, std::max(rx, 3), r, rim);
    if (rx > 6) cv.ellipse(cx, cy, rx - (int)(rx * 0.12) - 1, r - (int)(r * 0.12), face);
    if (rx > r / 2) {
        const char* t = heads ? "H" : "T";
        cv.textCentered(t, F_BIG, rim, cx, cy - Canvas::fontHeight(F_BIG) / 2);
    }
}

void App::drawCoinToss(Canvas& cv, uint64_t now) {
    int cx = PW / 2;
    uint64_t t = tossStart_ ? now - tossStart_ : 0;
    const double PI = 3.14159265358979;
    const int baseY = 250, R = 70;
    char call[48]; snprintf(call, sizeof call, "%s called %s", confirmMode_ == 0 ? "Player One" : "You", tossCall_ ? "tails" : "heads");
    cv.textCentered(call, F_LABEL, GRAY, cx, 24);
    if (t < (uint64_t)TOSS_MS) {
        double p = (double)t / TOSS_MS;
        double turns = 6;                                       // whole spins before landing
        double final = tossResult_ ? PI : 0.0;
        double ease = 1 - (1 - p) * (1 - p);                    // fast at the start, settles at the end
        double angle = ease * (turns * 2 * PI + final);
        int y = baseY - (int)(std::sin(p * PI) * 140);
        cv.ellipse(cx, baseY + R + 18, (int)(R * (0.6 + 0.4 * (1 - std::sin(p * PI)))), 8, rgb565(30, 30, 30));   // shadow
        drawCoin(cv, cx, y, R, angle);
        return;
    }
    cv.ellipse(cx, baseY + R + 18, R, 8, rgb565(30, 30, 30));
    drawCoin(cv, cx, baseY, R, tossResult_ ? PI : 0.0);
    cv.textCentered(tossResult_ ? "Tails!" : "Heads!", F_BIG, WHITE, cx, 70);
    const char* who = confirmMode_ != 0 ? (tossFirst_ ? "You go first" : "R.O.G.E.R goes first")
                                        : (tossFirst_ ? "Player Two goes first" : "Player One goes first");
    uint16_t col = confirmMode_ != 0 ? (tossFirst_ ? YELLOW : RED) : (tossFirst_ ? BLUE : RED);
    cv.textCentered(who, F_ITEM, col, cx, 350);
    hint(cv, SEL + ": start now");
}

// ---------------- game over ----------------
void App::startGameOver(uint64_t now) {
    overSince_ = now;
    endGroup_.clear();
    bool ai = b_.mode != 0 && b_.winner <= 1;
    if (ai && faces_.loaded()) {
        std::string g = groupFor(b_.winner == 0 ? FACE_WIN : FACE_LOSS);
        if (g != faces_.idle) {
            endGroup_ = g;
            if (isReaction(faces_.current)) { faces_.finishPassThen(g, 1); over_ = Over::WAIT_REACTION; }  // reaction ends its pass first
            else { faces_.play(g, 1); over_ = Over::GIF; }                                               // once, then idle
            faceDirty_ = true;
            return;
        }
    }
    over_ = Over::HOLD;              // 1v1, draw, or no win/loss GIF: show the final screen briefly
}

void App::advanceGameOver(uint64_t now) {
    if (b_.state != State::GAME_OVER) { over_ = Over::NONE; return; }
    if (over_ == Over::WAIT_REACTION && faces_.current == endGroup_) over_ = Over::GIF;
    else if (over_ == Over::GIF && faces_.current != endGroup_) over_ = Over::POPUP;          // played out
    else if (over_ == Over::HOLD && now - overSince_ >= (uint64_t)OVER_HOLD_MS) over_ = Over::POPUP;
    if ((over_ == Over::WAIT_REACTION || over_ == Over::GIF) && now - overSince_ > (uint64_t)OVER_MAX_MS) over_ = Over::POPUP;
}

void App::gameOverInput(Act a) {
    if (a != A_SELECT) return;
    if (over_ != Over::POPUP) { over_ = Over::POPUP; return; }   // skip straight to the results
    b_.onButton(BTN_CONFIRM);                                    // Play again: re-home, back to mode selection
}

void App::drawGameOverPopup(Canvas& cv) {
    bool ai = b_.mode != 0;
    if (ai && faces_.loaded()) {                                 // R.O.G.E.R stays behind the popup, dimmed
        cv.blitScaled(faces_.frame(), faces_.width, faces_.height, faces_.bakedRotate == 180);
        cv.rectAlpha(0, 0, PW, PH, BLACK, 200);
    }
    const char* title; const char* sub; uint16_t col;
    char subBuf[96];
    if (b_.winner == 2) { title = "DRAW"; sub = "Nobody wins this one."; col = WHITE; }
    else if (ai) {
        const char* diff = MODE_NAME[b_.mode];
        if (b_.winner == 0) { title = "YOU LOST"; snprintf(subBuf, sizeof subBuf, "R.O.G.E.R won on %s. Good game!", diff); col = RED; }
        else                { title = "YOU WON!"; snprintf(subBuf, sizeof subBuf, "You beat R.O.G.E.R on %s. Nice!", diff); col = YELLOW; }
        sub = subBuf;
    } else {
        title = b_.winner == 0 ? "PLAYER ONE WINS!" : "PLAYER TWO WINS!";
        sub = "Good game."; col = b_.winner == 0 ? RED : BLUE;
    }
    int cx = PW / 2;
    cv.roundRect(70, 70, PW - 140, PH - 140, 18, rgb565(22, 22, 26));
    cv.textCentered(title, F_TITLE, col, cx, 118);
    cv.textCentered(sub, F_LABEL, GRAY, cx, 196);
    drawItem(cv, 270, "Play again", true, cx, 320);
    cv.textCentered(SEL + ": back to mode selection", F_SMALL, DIM, cx, 340);
}

// ---------------- link debug ----------------
void App::openLinkDebug() {
    dbgSel_ = 0;
    screen = Screen::LINKDEBUG;
    if (b_.debugPing()) dbgLastPing_ = lastNow_;
}

void App::drawLinkDebug(Canvas& cv, uint64_t now) {
    const LinkCounters& lc = b_.linkCounters();
    const Brain::McStatus& mc = b_.mc;
    cv.text("Link debug", F_LABEL, WHITE, 16, 8);
    char buf[160];
    snprintf(buf, sizeof buf, "Pi state: %s   %s", stateName(b_.state), screenOnly() ? "(screen only: game uses the simulator)" : "");
    cv.textRight(buf, F_SMALL, GRAY, PW - 16, 14);

    int lh = Canvas::fontHeight(F_SMALL) + 6, y = 52;
    snprintf(buf, sizeof buf, "Pi sent: %u frames", (unsigned)lc.txFrames);                         cv.text(buf, F_SMALL, WHITE, 16, y);
    snprintf(buf, sizeof buf, "Pi got: %u frames, %u bytes", (unsigned)lc.rxFrames, (unsigned)lc.rxBytes); cv.text(buf, F_SMALL, WHITE, 16, y + lh);
    snprintf(buf, sizeof buf, "Corrupted at Pi: %u", (unsigned)lc.badFrames);
    cv.text(buf, F_SMALL, lc.badFrames ? YELLOW : WHITE, 16, y + 2 * lh);
    if (mc.lastFrame) snprintf(buf, sizeof buf, "Last from board: %.1f s ago", (now - mc.lastFrame) / 1000.0);
    else snprintf(buf, sizeof buf, "Last from board: never");
    cv.text(buf, F_SMALL, mc.lastFrame ? WHITE : RED, 16, y + 3 * lh);
    int rx = 420;
    bool hears = mc.lastHeard != 0;
    if (hears) snprintf(buf, sizeof buf, "Board hears Pi: YES (%.1f s ago)", (now - mc.lastHeard) / 1000.0);
    else if (b_.strayReady() > 0) snprintf(buf, sizeof buf, "Board hears Pi: NO (%d boot READYs)", b_.strayReady());
    else snprintf(buf, sizeof buf, "Board hears Pi: no reply yet");
    cv.text(buf, F_SMALL, hears ? GREEN : RED, rx, y);
    if (mc.fw >= 0) snprintf(buf, sizeof buf, "Firmware: v%d   last restart: %s", mc.fw, mc.resetReason >= 0 ? Brain::resetReasonName(mc.resetReason) : "?");
    else snprintf(buf, sizeof buf, "Firmware: unknown (v1?)");
    cv.text(fitText(buf, F_SMALL, PW - rx - 10), F_SMALL, mc.resetReason == 9 ? YELLOW : WHITE, rx, y + lh);
    if (mc.statusKnown) snprintf(buf, sizeof buf, "Homed: %s   limit switch: %s", mc.homed ? "yes" : "NO", mc.limit ? "pressed" : "open");
    else snprintf(buf, sizeof buf, "Homed: ?   limit switch: ?");
    cv.text(buf, F_SMALL, WHITE, rx, y + 2 * lh);
    if (mc.badFrames >= 0) snprintf(buf, sizeof buf, "Corrupted at board: %d", mc.badFrames); else snprintf(buf, sizeof buf, "Corrupted at board: ?");
    cv.text(buf, F_SMALL, mc.badFrames > 0 ? YELLOW : WHITE, rx, y + 3 * lh);

    const char* v; uint16_t vc = YELLOW;
    bool nothing = !lc.rxBytes && !lc.rxFrames;
    if (screenOnly() && nothing) { v = "Screen only is on. Nothing from the board yet (fine if it is unplugged)."; vc = GRAY; }
    else if (nothing) v = "Nothing from the board: check board GPIO5 -> Pi pin 10, ground, and that it is powered.";
    else if (!hears && b_.strayReady() > 0) { v = "Board talks but never hears the Pi: check Pi pin 8 -> board GPIO14."; vc = RED; }
    else if (!hears && mc.fw < 0) v = "Board talks. No answers yet: old firmware (v1) or Pi pin 8 -> GPIO14 broken.";
    else if (lc.badFrames || mc.badFrames > 0) v = "Link works but frames get corrupted: loose wire, noise or missing ground.";
    else { v = "Link OK in both directions."; vc = GREEN; }
    if (!gantryInfo.empty()) { v = gantryInfo.c_str(); vc = gantryInfo.find("NOT") != std::string::npos ? YELLOW : GREEN; }
    cv.text(fitText(v, F_SMALL, PW - 32), F_SMALL, vc, 16, y + 4 * lh + 4);

    int ly = y + 5 * lh + 14;
    cv.rect(16, ly - 6, PW - 32, 1, DIM);
    int rows = (420 - ly) / lh;
    int start = (int)b_.linkLog.size() - rows; if (start < 0) start = 0;
    for (int i = start; i < (int)b_.linkLog.size(); i++) {
        const auto& e = b_.linkLog[i];
        snprintf(buf, sizeof buf, "%8.1f  %s  %s", e.t / 1000.0, e.fromMc ? "board > Pi" : "Pi > board", e.text.c_str());
        cv.text(fitText(buf, F_SMALL, PW - 32), F_SMALL, e.fromMc ? WHITE : CYAN, 16, ly);
        ly += lh;
    }
    if (b_.linkLog.empty()) cv.text("(no traffic yet)", F_SMALL, DIM, 16, ly);

    char colLbl[24], dropLbl[24];
    snprintf(colLbl, sizeof colLbl, "Column %d", dbgCol_); snprintf(dropLbl, sizeof dropLbl, "Drop in %d", dbgCol_);
    const char* items[N_DBG] = { "Ping", "Home", colLbl, dropLbl, "Restart", "Back" };
    int bw = (PW - 32 - 5 * 8) / N_DBG, bx = 16, by = PH - 48;
    for (int i = 0; i < N_DBG; i++) {
        bool sel = i == dbgSel_;
        if (sel) cv.roundRect(bx, by, bw, 38, 8, WHITE); else cv.roundRectOutline(bx, by, bw, 38, 8, 2, DIM);
        cv.textCentered(items[i], F_SMALL, sel ? BLACK : WHITE, bx + bw / 2, by + (38 - Canvas::fontHeight(F_SMALL)) / 2);
        bx += bw + 8;
    }
    if (!b_.debugAllowed()) cv.textRight("(tools work from the menu or a fault, not mid-game)", F_SMALL, DIM, PW - 16, by - lh);
}


// ---------------- servo calibration (Pi GPIO gantry) ----------------
static const char* CAL_NAME[6] = { "Rest", "Red select", "Red push", "Black select", "Black push", "Wait after move" };
static const int N_CAL = 11;   // 6 values, Test red, Test black, Check signal, Save, Back

void App::openServoCal() {
    calSel_ = 0; calEditing_ = false; calSaveSeen_ = 0;
    if (mc_) {
        ServoCal c = mc_->status().cal;
        int v[6] = {c.rest, c.redSel, c.redPush, c.blackSel, c.blackPush, c.waitMs};
        for (int i = 0; i < 6; i++) calVals_[i] = v[i];
        mc_->calBegin();
    }
    screen = Screen::SERVOCAL;
}

void App::closeServoCal() {
    if (mc_) mc_->calEnd();
    calEditing_ = false;
    screen = b_.state == State::FAULT ? Screen::GAME : Screen::OPTIONS;
}

void App::servoCalInput(Act a) {
    if (!mc_) { if (a == A_SELECT || a == A_BACK) closeServoCal(); return; }
    if (calEditing_) {
        if (a == A_UP || a == A_DOWN) {           // left / right = minus / plus
            int step = calSel_ == 5 ? 10 : 1;
            int v = calVals_[calSel_] + (a == A_DOWN ? step : -step);
            if (calSel_ == 5) v = std::max(50, std::min(3000, v)); else v = std::max(0, std::min(180, v));
            calVals_[calSel_] = v;
            mc_->calSet(calSel_, v);              // stores it and moves the servo there (live)
        } else if (a == A_SELECT || a == A_BACK) calEditing_ = false;
        return;
    }
    if (a == A_UP) calSel_ = (calSel_ + N_CAL - 1) % N_CAL;
    else if (a == A_DOWN) calSel_ = (calSel_ + 1) % N_CAL;
    else if (a == A_BACK) closeServoCal();
    else if (a == A_SELECT) {
        if (calSel_ <= 5) { calEditing_ = true; if (calSel_ < 5) mc_->calSet(calSel_, calVals_[calSel_]); }   // show this position now
        else if (calSel_ == 6) mc_->calTest(0);
        else if (calSel_ == 7) mc_->calTest(1);
        else if (calSel_ == 8) mc_->calCheckSignal();
        else if (calSel_ == 9) mc_->calSave();
        else closeServoCal();
    }
}

void App::drawServoCal(Canvas& cv) {
    int cx = PW / 2;
    cv.textCentered("Servo calibration", F_BIG, WHITE, cx, 18);
    if (!mc_) {
        cv.textCentered("No gantry on this Pi's GPIO in this mode.", F_LABEL, GRAY, cx, 160);
        cv.textCentered("Start the game with --gpio (the normal Pi 4 setup) to calibrate the servo.", F_SMALL, DIM, cx, 210);
        drawItem(cv, 300, "Back", true, cx);
        hint(cv, SEL + ": back");
        return;
    }
    MotionController::Status st = mc_->status();
    const int lx = 205, rx = 595, top = 88, step = 50, bw = 360;
    char buf[64];
    for (int i = 0; i < 6; i++) {
        bool sel = i == calSel_, ed = sel && calEditing_;
        if (i == 5) snprintf(buf, sizeof buf, ed ? "< %s: %d ms >" : "%s: %d ms", CAL_NAME[i], calVals_[i]);
        else snprintf(buf, sizeof buf, ed ? "< %s: %d deg >" : "%s: %d deg", CAL_NAME[i], calVals_[i]);
        int y = top + i * step;
        if (ed) { cv.roundRect(lx - bw / 2, y, bw, BOX_H, 10, YELLOW); cv.textCentered(buf, F_LABEL, BLACK, lx, y + (BOX_H - Canvas::fontHeight(F_LABEL)) / 2); }
        else if (sel) { cv.roundRect(lx - bw / 2, y, bw, BOX_H, 10, WHITE); cv.textCentered(buf, F_LABEL, BLACK, lx, y + (BOX_H - Canvas::fontHeight(F_LABEL)) / 2); }
        else cv.textCentered(buf, F_LABEL, WHITE, lx, y + (BOX_H - Canvas::fontHeight(F_LABEL)) / 2);
    }
    const char* act[5] = { "Test red drop", "Test black drop", "Check signal", "Save", "Back" };
    for (int i = 0; i < 5; i++) drawItem(cv, top + 4 + i * 46, act[i], calSel_ == 6 + i, rx, 300);
    std::string save = st.saveResult == 1 ? "Saved: kept after power-off" : st.saveResult == -1 ? "SAVE FAILED (see ~/c4_log.txt)" : "Not saved yet: choose Save to keep these";
    cv.textCentered(save, F_SMALL, st.saveResult == 1 ? GREEN : st.saveResult == -1 ? RED : YELLOW, rx, top + 4 + 5 * 46 + 4);
    if (!st.signal.empty()) {
        bool ok = st.signal.find("IS sending") != std::string::npos || st.signal.find("- OK") != std::string::npos;
        cv.textCentered(fitText("Signal: " + st.signal, F_SMALL, PW - 40), F_SMALL, ok ? GREEN : RED, cx, PH - 98);
    }
    std::string info = "Servo: " + st.servoInfo + (st.busy ? "   (gantry busy, waiting...)" : "");
    cv.textCentered(fitText(info, F_SMALL, PW - 40), F_SMALL, DIM, cx, PH - 74);
    hint(cv, calEditing_ ? "Left/Right: -/+ (servo moves live)    " + SEL + ": done"
                         : "Left/Right: move    " + SEL + ": edit / do it");
}
