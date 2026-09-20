#pragma once
#include <stdint.h>
#include <limits.h>
#include "board.h"

// ============================================================
// minimax.h -- Bitboard Alpha-Beta Search
// ============================================================
// C port of the Python test suite's h_alphabeta_search, kept
// structurally identical (separate max_value/min_value, same
// center-first move ordering, same terminal-value discounting) so
// behavior stays traceable back to the validated Python reference
// rather than diverging into a differently-shaped algorithm.
//
// KEY DIFFERENCE FROM THE PYTHON VERSION: terminal/heuristic values
// use a large-magnitude INTEGER scale instead of floats. A proven
// win/loss is scored as +/-(C4_TERMINAL_BIG - depth), which is always
// far outside the heuristic's normal range (a few hundred points at
// most) -- so terminal outcomes always correctly outrank/outrank
// heuristic estimates in comparisons, with no floating point needed
// at all. This matters for embedded: integer comparisons are cheap
// and deterministic; float comparisons on some ESP32 cores are not
// free, and floats invite subtle equality/rounding bugs that integers
// don't have.
//
// CORRECTED DEPTH-DISCOUNT FORMULA (see conversation): a WIN scores
// HIGHER the sooner it's found (prefer fast wins); a LOSS scores
// HIGHER (less negative) the LATER it's found (delay unavoidable
// losses, force the opponent to prove it). Both fall out of a single
// formula: score = raw * (BIG - depth).

#define C4_TERMINAL_BIG 1000000   // far above any realistic heuristic
                                    // magnitude (heuristic tops out in
                                    // the low hundreds) -- guarantees
                                    // terminal outcomes are never
                                    // confused with heuristic estimates

static const uint8_t CENTER_FIRST_ORDER[C4_WIDTH] = {3, 2, 4, 1, 5, 0, 6};

// ------------------------------------------------------------
// Precomputed heuristic windows
// ------------------------------------------------------------
// Every possible 4-in-a-row "window" on the board, as a bitmask, built
// ONCE at startup rather than rebuilt on every heuristic call -- this
// is the single biggest reason the C version will be fast where the
// Python version wasn't: evaluating a window here is one AND + one
// popcount instead of a list comprehension over board lookups.
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

// ------------------------------------------------------------
// c4_heuristic: Positional evaluation, mirrors Python's window_score
// ------------------------------------------------------------
#define C4_CENTER_COL_MASK (0x3FULL << (3 * 7))  // rows 0-5 of column 3

static int32_t c4_heuristic(const C4State* s, C4Player player) {
    C4Player opp = (player == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    uint64_t pbb = s->bitboard[player];
    uint64_t obb = s->bitboard[opp];

    int32_t score = __builtin_popcountll(pbb & C4_CENTER_COL_MASK) * 6;

    for (int i = 0; i < C4_NUM_WINDOWS; i++) {
        uint64_t mask = g_window_masks[i];
        int p = __builtin_popcountll(pbb & mask);
        int o = __builtin_popcountll(obb & mask);
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
}

// ------------------------------------------------------------
// Terminal detection: derived fresh from bitboards, not cached
// ------------------------------------------------------------
static inline bool c4_is_terminal(const C4State* s) {
    C4Player last_mover = (s->to_move == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    return c4_is_win(s->bitboard[last_mover]) || c4_is_full(s);
}

static inline int32_t c4_utility(const C4State* s, C4Player player) {
    C4Player last_mover = (s->to_move == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    if (c4_is_win(s->bitboard[last_mover])) {
        return (last_mover == player) ? 1 : -1;
    }
    return 0; // board full, nobody won: draw
}

static C4Player g_search_player; // fixed for the duration of one top-level call

static inline int32_t c4_terminal_value(const C4State* s, uint8_t depth) {
    int32_t raw = c4_utility(s, g_search_player);
    if (raw == 0) return 0;
    // CORRECTED formula: raw * (BIG - depth). For a win (raw=+1), this
    // decreases as depth grows -- prefer faster wins. For a loss
    // (raw=-1), this becomes LESS negative as depth grows -- prefer
    // slower losses. Both directions fall out of the single formula.
    return raw * (int32_t)(C4_TERMINAL_BIG - depth);
}

// ------------------------------------------------------------
// max_value / min_value: same structure as the Python reference
// ------------------------------------------------------------
// State is passed BY VALUE (C4State is small -- two uint64_t, 7
// bytes, one enum) so each recursive call works on its own copy
// without needing explicit undo logic. Simpler and less error-prone
// than make/unmake; revisit only if profiling on real hardware shows
// the copies actually matter, which is unlikely at this state size.

static int32_t min_value(C4State s, int32_t alpha, int32_t beta,
                          uint8_t depth, uint8_t max_depth);

static int32_t max_value(C4State s, int32_t alpha, int32_t beta,
                          uint8_t depth, uint8_t max_depth, uint8_t* out_move) {
    extern uint32_t g_max_calls;
    g_max_calls++;

    if (c4_is_terminal(&s)) return c4_terminal_value(&s, depth);
    if (depth >= max_depth) return c4_heuristic(&s, g_search_player);

    int32_t v = INT32_MIN;
    uint8_t best_move = 0xFF;
    for (int i = 0; i < C4_WIDTH; i++) {
        uint8_t col = CENTER_FIRST_ORDER[i];
        if (!c4_is_column_legal(&s, col)) continue;

        C4State next = s;
        c4_apply_move(&next, col);
        int32_t v2 = min_value(next, alpha, beta, depth + 1, max_depth);

        if (v2 > v) { v = v2; best_move = col; }
        if (v > alpha) alpha = v;
        if (v >= beta) break;
    }
    if (out_move) *out_move = best_move;
    return v;
}

static int32_t min_value(C4State s, int32_t alpha, int32_t beta,
                          uint8_t depth, uint8_t max_depth) {
    extern uint32_t g_min_calls;
    g_min_calls++;

    if (c4_is_terminal(&s)) return c4_terminal_value(&s, depth);
    if (depth >= max_depth) return c4_heuristic(&s, g_search_player);

    int32_t v = INT32_MAX;
    for (int i = 0; i < C4_WIDTH; i++) {
        uint8_t col = CENTER_FIRST_ORDER[i];
        if (!c4_is_column_legal(&s, col)) continue;

        C4State next = s;
        c4_apply_move(&next, col);
        int32_t v2 = max_value(next, alpha, beta, depth + 1, max_depth, nullptr);

        if (v2 < v) v = v2;
        if (v < beta) beta = v;
        if (v <= alpha) break;
    }
    return v;
}

// ------------------------------------------------------------
// h_alphabeta_search: public entry point
// ------------------------------------------------------------
uint32_t g_max_calls = 0;
uint32_t g_min_calls = 0;

typedef struct {
    uint8_t move;
    int32_t value;
    uint32_t nodes;
} SearchResult;

SearchResult h_alphabeta_search(C4State root, uint8_t max_depth) {
    c4_init_windows();
    g_search_player = root.to_move;
    g_max_calls = 0;
    g_min_calls = 0;

    uint8_t move = 0xFF;
    int32_t value = max_value(root, INT32_MIN, INT32_MAX, 0, max_depth, &move);

    SearchResult result;
    result.move = move;
    result.value = value;
    result.nodes = g_max_calls + g_min_calls;
    return result;
}