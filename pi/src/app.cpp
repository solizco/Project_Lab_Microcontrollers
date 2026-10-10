#include "app.h"
#include <cstdio>
#include <algorithm>
#include <random>

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
static const int N_OPT = 4;   // Screen only toggle, GIF test, Shut down, Back
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
            if (b_.startGame(confirmMode_)) {
                screen = Screen::GAME;
                lastState_ = b_.state;
                faceDirty_ = true;
                if (faces_.loaded()) faces_.play(faces_.idle, 0);
            } else {
                notice_ = b_.state == State::HOMING ? "The gantry is still homing. Try again in a moment."
                                                    : "Can't start right now.";
            }
        }
        break;
    case Screen::OPTIONS:
        if (a == A_UP) optSel_ = (optSel_ + N_OPT - 1) % N_OPT;
        else if (a == A_DOWN) optSel_ = (optSel_ + 1) % N_OPT;
        else if (a == A_BACK) screen = b_.state == State::FAULT ? Screen::GAME : Screen::MENU;
        else if (a == A_SELECT) {
            if (optSel_ == 0 && sw_) { smSel_ = screenOnly() ? screenInput_ : 0; screen = Screen::SCREENMODE; }
            else if (optSel_ == 1) { screen = Screen::GIFTEST; if (faces_.loaded()) faces_.playClip(0, 0); }
            else if (optSel_ == 2) { shutSel_ = 1; screen = Screen::SHUTDOWN; }
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
    case Screen::GAME:   // only reached in FAULT: select opens Options (screen only / shut down)
        if (a == A_SELECT) { optSel_ = 0; screen = Screen::OPTIONS; }
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
        WIN = {"roger_win", "win"}, LOSS = {"roger_loss", "loss", "lose"}, THINK = {"thinking", "think"};
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

void App::applyFace(FaceEvent f) {
    if (!faces_.loaded()) return;
    switch (f) {
    case FACE_IDLE:
        // let a one-shot reaction finish on its own; it falls back to idle by itself
        if (!(faces_.oneShotPlaying() && faces_.current != faces_.idle)) faces_.play(faces_.idle, 0);
        break;
    case FACE_THINKING: faces_.play(groupFor(f), 0); break;          // loop while thinking
    case FACE_GOOD_MOVE:
    case FACE_BAD_MOVE: faces_.play(groupFor(f), 1); break;          // once, then idle
    case FACE_WIN:
    case FACE_LOSS:     faces_.play(groupFor(f), 0); break;          // loop until the next game
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
        if (s == State::GAME_OVER) recordResult();
        if (screen == Screen::GAME && (s == State::HOMING || s == State::MENU)) {
            screen = Screen::MENU;               // game over -> re-home -> back to the menu
            sel_ = b_.menuIndex;
        }
        if (s == State::FAULT && screen != Screen::SHUTDOWN) screen = Screen::GAME;
        lastState_ = s;
    }
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
    extra += "|" + std::to_string(smSel_) + (wrongInputAt_ && now - wrongInputAt_ < 2500 ? "|w" : "|");
    if (screenOnly() && b_.state == State::AWAITING_DROP) extra += std::to_string((now - dropStart_) / 16);   // ~60 fps fall
    return std::string(buf) + statusText(now) + extra;
}

bool App::frame(uint64_t now, Canvas& cv) {
    lastNow_ = now;
    syncWithBrain(now);
    bool faceOnScreen = (screen == Screen::GAME && b_.mode != 0 && b_.state != State::FAULT) || screen == Screen::GIFTEST;
    bool changed = false;
    if (faceOnScreen && faces_.loaded()) changed = faces_.update(now);
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
    case Screen::MENU:     drawMenu(cv); break;
    case Screen::CONFIRM:  drawConfirm(cv); break;
    case Screen::OPTIONS:  drawOptions(cv); break;
    case Screen::SCREENMODE: drawScreenMode(cv); break;
    case Screen::SHUTDOWN: drawShutdown(cv); break;
    case Screen::STATS:    drawStats(cv); break;
    case Screen::GIFTEST:  drawGifTest(cv); break;
    case Screen::GAME:     drawGame(cv, now); break;
    }
}

void App::drawMenu(Canvas& cv) {
    int th = Canvas::fontHeight(F_TITLE);
    int ty = (PH - 40 - th) / 2;
    cv.textCentered("CONNECT 4", F_TITLE, WHITE, TITLE_CX, ty);
    // gantry status (not in Finn's sketch: the game can't start until homing is done)
    const char* g = screenOnly() ? (screenInput_ ? "screen only - Motion Controller buttons" : "screen only - keyboard") :
                    b_.state == State::HOMING ? "gantry homing..." : b_.state == State::FAULT ? "gantry fault" : "gantry ready";
    cv.textCentered(g, F_SMALL, screenOnly() ? YELLOW : b_.state == State::MENU ? GRAY : DIM, TITLE_CX, ty + th + 14);

    int h = 0;
    for (const auto& e : MENU) h += e.kind == 'G' ? GAP : e.kind == 'L' ? LABEL_STEP : ITEM_STEP;
    int y = std::max(10, (PH - 40 - h) / 2);
    int item = 0;
    for (const auto& e : MENU) {
        if (e.kind == 'G') y += GAP;
        else if (e.kind == 'L') { cv.textCentered(e.text, F_LABEL, GRAY, MENU_CX, y); y += LABEL_STEP; }
        else { drawItem(cv, y, e.text, item == sel_, MENU_CX); y += ITEM_STEP; item++; }
    }
    hint(cv, "Left/Right: move    Select: choose");
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
    hint(cv, "Left/Right: move    Select: choose");
}

void App::drawOptions(Canvas& cv) {
    int cx = PW / 2;
    cv.textCentered("Options", F_BIG, WHITE, cx, 30);
    int y = 120;
    const char* items[N_OPT] = { !screenOnly() ? "Screen only: OFF" : screenInput_ ? "Screen only: Buttons" : "Screen only: Keyboard",
                                 "GIF test", "Shut down", "Back" };
    for (int i = 0; i < N_OPT; i++) { drawItem(cv, y, items[i], i == optSel_, cx, 380); y += ITEM_STEP; }
    if (!sw_) cv.textCentered("(screen only is not available in this build)", F_SMALL, DIM, cx, y + 16);
    else if (screenOnly()) cv.textCentered(screenInput_ ? "Games play on the display with the Motion Controller buttons. Gantry parked."
                                                        : "Games play on the display with the Pi's keyboard. Gantry parked.", F_SMALL, GRAY, cx, y + 16);
    else cv.textCentered("Turn on to play a whole game on the display, without the gantry.", F_SMALL, DIM, cx, y + 16);
    cv.textCentered("More options TBD", F_LABEL, DIM, cx, y + 56);
    hint(cv, "Left/Right: move    Select: choose");
}

void App::drawShutdown(Canvas& cv) {
    int cx = PW / 2, top = 70;
    cv.textCentered("Shut down?", F_BIG, WHITE, cx, top + 40);
    cv.textCentered("Always shut down before unplugging,", F_LABEL, GRAY, cx, top + 130);
    cv.textCentered("or the SD card can get corrupted.", F_LABEL, GRAY, cx, top + 165);
    drawItem(cv, top + 220, "Shut down", shutSel_ == 0, cx);
    drawItem(cv, top + 220 + ITEM_STEP, "Cancel", shutSel_ == 1, cx);
    hint(cv, "Left/Right: move    Select: choose");
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
    hint(cv, "Select: back    (keyboard: T test game, C clear)");
}

void App::drawGifTest(Canvas& cv) {
    if (!faces_.loaded()) {
        cv.textCentered("GIF test", F_BIG, WHITE, PW / 2, 100);
        cv.textCentered("No faces loaded.", F_LABEL, GRAY, PW / 2, 200);
        cv.textCentered("Run convert.bat on the laptop and copy the faces", F_SMALL, DIM, PW / 2, 250);
        cv.textCentered("folder onto the SD card (see README).", F_SMALL, DIM, PW / 2, 275);
        hint(cv, "Select: back");
        return;
    }
    cv.blitScaled(faces_.frame(), faces_.width, faces_.height, faces_.bakedRotate == 180);
    char buf[160];
    snprintf(buf, sizeof buf, "%d/%zu  %s  (%s)", faces_.clipIndex + 1, faces_.clips.size(),
             faces_.clips[faces_.clipIndex].name.c_str(), faces_.current.c_str());
    int w1 = Canvas::textWidth(buf, F_SMALL), w2 = Canvas::textWidth("Left/Right: prev/next   Select: back", F_SMALL);
    cv.rectAlpha(6, 6, std::max(w1, w2) + 16, 52, BLACK, 190);
    cv.text(buf, F_SMALL, WHITE, 14, 10);
    cv.text("Left/Right: prev/next   Select: back", F_SMALL, GRAY, 14, 32);
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
    cv.textCentered("MOTION FAULT", F_BIG, WHITE, PW / 2, 90);
    char buf[64]; snprintf(buf, sizeof buf, "code 0x%02X", b_.faultCode);
    cv.textCentered(buf, F_LABEL, WHITE, PW / 2, 170);
    const char* why = b_.faultCode == 0xFF ? "The boards are out of sync (wrong column confirmed)." :
                      b_.faultCode == 0xFD ? "No reply after a drop. Check whether the piece fell." :
                      b_.faultCode == 0x01 ? "The gantry stalled or could not find home." :
                      b_.faultCode == 0x04 ? "A drop was requested before homing." : "The Motion Controller reported an error.";
    cv.textCentered(why, F_SMALL, WHITE, PW / 2, 230);
    cv.textCentered("Check the mechanism, then power-cycle.", F_SMALL, WHITE, PW / 2, 260);
    cv.textCentered("Or play on the screen only: Options > Screen only.", F_SMALL, WHITE, PW / 2, 300);
    hint(cv, "Select: options");
}

void App::drawPvp(Canvas& cv) {
    bool over = b_.state == State::GAME_OVER;
    int player = b_.board.toMove();
    if (over) {
        const char* t = b_.winner == 2 ? "Draw" : b_.winner == 0 ? "Player One wins!" : "Player Two wins!";
        uint16_t c = b_.winner == 0 ? RED : b_.winner == 1 ? BLUE : WHITE;
        cv.circle(PW / 4 + 20, PH / 2 - 20, 110, c);
        cv.textCentered(t, F_BIG, c, PW * 5 / 8 + 20, PH / 2 - 75);
        hint(cv, "Select: play again");
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
    if (screenOnly()) { drawScreenOnly(cv, now); return; }
    if (b_.mode == 0) { drawPvp(cv); if (showBoard) drawMiniBoard(cv); return; }

    if (faces_.loaded()) cv.blitScaled(faces_.frame(), faces_.width, faces_.height, faces_.bakedRotate == 180);
    else {
        cv.textCentered("R.O.G.E.R", F_TITLE, WHITE, PW / 2, 120);
        cv.textCentered("(no face files loaded, see Options > GIF test)", F_SMALL, DIM, PW / 2, 200);
    }

    if (b_.state == State::GAME_OVER) {
        const char* t = b_.winner == 2 ? "DRAW" : b_.winner == 0 ? "R.O.G.E.R WINS" : "YOU WIN!";
        uint16_t c = b_.winner == 0 ? RED : b_.winner == 1 ? YELLOW : WHITE;
        cv.rectAlpha(0, PH - 130, PW, 130, BLACK, 170);
        cv.textCentered(t, F_BIG, c, PW / 2, PH - 120);
        cv.textCentered("Select: play again", F_LABEL, GRAY, PW / 2, PH - 50);
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
            cv.textCentered("Select: play again", F_LABEL, GRAY, RCX, 395);
        }
    } else {
        if (over) {
            const char* t = b_.winner == 2 ? "Draw" : b_.winner == 0 ? "Player One wins!" : "Player Two wins!";
            uint16_t c = b_.winner == 0 ? RED : b_.winner == 1 ? BLUE : WHITE;
            cv.circle(RCX, 150, 70, c);
            cv.textCentered(t, F_ITEM, c, RCX, 250);
            cv.textCentered("Select: play again", F_LABEL, GRAY, RCX, 320);
        } else {
            int p = b_.board.toMove();
            uint16_t c = p == 0 ? RED : BLUE;
            cv.circle(RCX, 150, 70, c);
            cv.textCentered(p == 0 ? "Player One's" : "Player Two's", F_ITEM, c, RCX, 240);
            cv.textCentered("turn", F_ITEM, c, RCX, 285);
        }
        cv.textCentered("1v1  -  screen only", F_LABEL, GRAY, RCX, 40);
    }
    if (!over) cv.textCentered(screenInput_ ? "Buttons: left/right = column, select = drop"
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
    hint(cv, "Left/Right: move    Select: choose");
}
