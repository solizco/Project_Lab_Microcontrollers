// ============================================================
// ai.cpp -- Bitboard alpha-beta search (Raspberry Pi port)
// ============================================================
// Port of the ESP32 minimax.h. The search, move ordering, heuristic
// weights, window masks and terminal scoring are unchanged (they were
// validated on the ESP32 and in the Python suite). What changed:
//   - Lives in ONE translation unit (the old header defined non-inline
//     globals, which breaks a multi-file Makefile).
//   - A cancel flag is polled every 1024 nodes. On cancel the whole
//     recursion unwinds and the result is marked aborted, with
//     move = 0xFF. A half-searched move is NEVER returned as an answer.
//   - No Arduino, no heartbeat hook.
// Keep popcount64_swar: the ARMv6 core in the Pi Zero has no hardware
// popcount instruction either, and the builtin becomes a function call.
//
// THREADING: the search state below is file-static, so only ONE search
// may run at a time. The game only ever runs one.
#pragma GCC optimize("O2")

#include "ai.h"
#include <limits.h>

static const uint8_t CENTER_FIRST_ORDER[C4_WIDTH] = {3, 2, 4, 1, 5, 0, 6};

#define C4_NUM_WINDOWS 69  // 24 horizontal + 21 vertical + 12 diag "/" + 12 diag "\"

static uint64_t g_window_masks[C4_NUM_WINDOWS];
static bool g_windows_initialized = false;

static void c4_init_windows() {
    if (g_windows_initialized) return;
    int idx = 0;

    for (int y = 0; y < C4_HEIGHT; y++)               // horizontal
        for (int x = 0; x <= C4_WIDTH - 4; x++) {
            uint64_t m = 0;
            for (int i = 0; i < 4; i++) m |= 1ULL << ((x + i) * 7 + y);
            g_window_masks[idx++] = m;
        }
    for (int x = 0; x < C4_WIDTH; x++)                 // vertical
        for (int y = 0; y <= C4_HEIGHT - 4; y++) {
            uint64_t m = 0;
            for (int i = 0; i < 4; i++) m |= 1ULL << (x * 7 + (y + i));
            g_window_masks[idx++] = m;
        }
    for (int x = 0; x <= C4_WIDTH - 4; x++)            // diagonal "/"
        for (int y = 0; y <= C4_HEIGHT - 4; y++) {
            uint64_t m = 0;
            for (int i = 0; i < 4; i++) m |= 1ULL << ((x + i) * 7 + (y + i));
            g_window_masks[idx++] = m;
        }
    for (int x = 0; x <= C4_WIDTH - 4; x++)            // diagonal "\"
        for (int y = 0; y <= C4_HEIGHT - 4; y++) {
            uint64_t m = 0;
            for (int i = 0; i < 4; i++) m |= 1ULL << ((x + i) * 7 + (y + 3 - i));
            g_window_masks[idx++] = m;
        }

    g_windows_initialized = true; // idx == C4_NUM_WINDOWS here, by construction
}


static inline __attribute__((always_inline)) int popcount64_swar(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}


#ifndef C4_DISABLE_HEURISTIC
#define C4_DISABLE_HEURISTIC 0
#endif

#define C4_CENTER_COL_MASK (0x3FULL << (3 * 7))  // rows 0-5 of column 3

static int32_t c4_heuristic(const C4State* s, C4Player player) {
#if C4_DISABLE_HEURISTIC
    return 0;
#else
    C4Player opp = (player == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    uint64_t pbb = s->bitboard[player];
    uint64_t obb = s->bitboard[opp];
    uint64_t all = pbb | obb;

    int32_t score = popcount64_swar(pbb & C4_CENTER_COL_MASK) * 6;

    for (int i = 0; i < C4_NUM_WINDOWS; i++) {
        uint64_t mask = g_window_masks[i];
        if ((all & mask) == 0) continue; // completely empty window --
                                            // skip both popcounts, it
                                            // can't contribute a score
                                            // either way
        int p = popcount64_swar(pbb & mask);
        int o = popcount64_swar(obb & mask);
        if (p > 0 && o > 0) continue; // window blocked by both -- worthless to either side
        if (o == 3)      score -= 80;
        else if (p == 3) score += 50;
        else if (o == 2) score -= 4;
        else if (p == 2) score += 5;
        // p==4 isn't handled here -- a real 4-in-a-row is caught by
        // c4_is_terminal() before the heuristic is ever called, same
        // as the Python version's reasoning
    }
    return score;
#endif
}


// ------------------------------------------------------------
// Terminal helpers + move ordering
// ------------------------------------------------------------
static int order_legal_moves(const C4State* s, uint8_t* out_moves) {
    uint8_t moves[C4_WIDTH];
    int32_t scores[C4_WIDTH];
    int n = 0;
    C4Player mover = s->to_move;

    for (int i = 0; i < C4_WIDTH; i++) {
        uint8_t col = CENTER_FIRST_ORDER[i]; // base order before ranking;
                                                // also the tiebreak order
                                                // for equally-scored moves
        if (!c4_is_column_legal(s, col)) continue;

        C4State next = *s;
        c4_apply_move(&next, col);

        int32_t score;
        if (c4_is_win(next.bitboard[mover])) {
            score = INT32_MAX; // immediate win -- always ranked first
        } else {
            score = c4_heuristic(&next, mover);
        }

        moves[n] = col;
        scores[n] = score;
        n++;
    }

    // Insertion sort, descending by score. n is at most 7 -- not worth
    // anything fancier. Stable (keeps center-first order among ties),
    // since it only swaps when strictly greater.
    for (int i = 1; i < n; i++) {
        uint8_t m = moves[i];
        int32_t sc = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < sc) {
            moves[j + 1] = moves[j];
            scores[j + 1] = scores[j];
            j--;
        }
        moves[j + 1] = m;
        scores[j + 1] = sc;
    }

    for (int i = 0; i < n; i++) out_moves[i] = moves[i];
    return n;
}


// ------------------------------------------------------------
// Terminal detection (derived from bitboards, not cached)
// ------------------------------------------------------------
static inline bool c4_is_terminal(const C4State* s) {
    C4Player last_mover = (s->to_move == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    return c4_is_win(s->bitboard[last_mover]) || c4_is_full(s);
}

static inline int32_t c4_utility(const C4State* s, C4Player player) {
    C4Player last_mover = (s->to_move == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    if (c4_is_win(s->bitboard[last_mover])) return (last_mover == player) ? 1 : -1;
    return 0; // board full, nobody won
}

// Per-search state (file-static: one search at a time, see header note)
static C4Player g_search_player;
static uint32_t g_nodes;
static const std::atomic<bool>* g_cancel;
static bool g_aborted;

// raw * (BIG - depth): wins found sooner score higher, losses found
// later score higher (less negative). Same formula as the ESP32 build.
static inline int32_t c4_terminal_value(const C4State* s, uint8_t depth) {
    int32_t raw = c4_utility(s, g_search_player);
    if (raw == 0) return 0;
    return raw * (int32_t)(C4_TERMINAL_BIG - depth);
}

// Poll the cancel flag every 1024 nodes. Once set, g_aborted stays set
// for the rest of this search and every node returns immediately, so
// the recursion collapses in a handful of microseconds.
static inline bool should_abort() {
    if ((++g_nodes & 0x3FF) == 0 && g_cancel &&
        g_cancel->load(std::memory_order_relaxed)) {
        g_aborted = true;
    }
    return g_aborted;
}

static int32_t min_value(C4State s, int32_t alpha, int32_t beta,
                         uint8_t depth, uint8_t max_depth);

static int32_t max_value(C4State s, int32_t alpha, int32_t beta,
                         uint8_t depth, uint8_t max_depth, uint8_t* out_move) {
    if (should_abort()) return 0;
    if (c4_is_terminal(&s)) return c4_terminal_value(&s, depth);
    if (depth >= max_depth) return c4_heuristic(&s, g_search_player);

    uint8_t ordered[C4_WIDTH];
    int n = order_legal_moves(&s, ordered);

    int32_t v = INT32_MIN;
    uint8_t best_move = 0xFF;
    for (int i = 0; i < n; i++) {
        uint8_t col = ordered[i];
        C4State next = s;
        c4_apply_move(&next, col);
        int32_t v2 = min_value(next, alpha, beta, depth + 1, max_depth);
        if (g_aborted) return 0;

        if (v2 > v) { v = v2; best_move = col; }
        if (v > alpha) alpha = v;
        if (v >= beta) break;
    }
    if (out_move) *out_move = best_move;
    return v;
}

static int32_t min_value(C4State s, int32_t alpha, int32_t beta,
                         uint8_t depth, uint8_t max_depth) {
    if (should_abort()) return 0;
    if (c4_is_terminal(&s)) return c4_terminal_value(&s, depth);
    if (depth >= max_depth) return c4_heuristic(&s, g_search_player);

    uint8_t ordered[C4_WIDTH];
    int n = order_legal_moves(&s, ordered);

    int32_t v = INT32_MAX;
    for (int i = 0; i < n; i++) {
        uint8_t col = ordered[i];
        C4State next = s;
        c4_apply_move(&next, col);
        int32_t v2 = max_value(next, alpha, beta, depth + 1, max_depth, nullptr);
        if (g_aborted) return 0;

        if (v2 < v) v = v2;
        if (v < beta) beta = v;
        if (v <= alpha) break;
    }
    return v;
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
static const bool g_windows_ready = (c4_init_windows(), true);

int32_t ai_heuristic(const C4State* s, C4Player player) {
    (void)g_windows_ready;
    return c4_heuristic(s, player);
}

SearchResult h_alphabeta_search(C4State root, uint8_t max_depth,
                                const std::atomic<bool>* cancel) {
    c4_init_windows();
    g_search_player = root.to_move;
    g_nodes = 0;
    g_cancel = cancel;
    g_aborted = false;

    uint8_t move = 0xFF;
    int32_t value = max_value(root, INT32_MIN, INT32_MAX, 0, max_depth, &move);

    SearchResult r;
    r.aborted = g_aborted;
    r.move = g_aborted ? 0xFF : move;
    r.value = g_aborted ? 0 : value;
    r.nodes = g_nodes;
    g_cancel = nullptr;
    return r;
}
