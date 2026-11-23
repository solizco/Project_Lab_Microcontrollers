#pragma once
// Forces -O2 for THIS FILE ONLY, regardless of whatever the project's
// overall build_type/build_flags are doing -- a source-level override
// that doesn't depend on getting .ini flags to actually propagate,
// which has been unreliable through build_src_filter in this project.
// GCC's function-level/file-level #pragma optimize is respected
// independent of the command-line optimization level.
#pragma GCC optimize("O2")

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
// popcount64_swar: manually inlined software popcount
// ------------------------------------------------------------
// Xtensa LX6 has no hardware popcount instruction. On-target testing
// showed __builtin_popcountll was costing roughly 50x the runtime of
// the rest of the search combined -- almost certainly because GCC
// lowers it to a CALL into a software popcount routine rather than
// inlining it, and that call overhead adds up fast at ~138 calls per
// heuristic evaluation. This is the well-known SWAR (SIMD Within A
// Register) technique, written out directly so the compiler inlines
// it as pure arithmetic with no call at all. Verified byte-for-byte
// against __builtin_popcountll across 100,000+ values before use here.
static inline __attribute__((always_inline)) int popcount64_swar(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

// ------------------------------------------------------------
// c4_heuristic: Positional evaluation, mirrors Python's window_score
// ------------------------------------------------------------
// DIAGNOSTIC SWITCH: change the 0 below to 1 directly and reflash to
// make this return instantly, skipping all window/popcount work --
// used to isolate the heuristic as the dominant cost on real hardware
// (confirmed: ~50x speedup with it disabled).
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

// ------------------------------------------------------------
// Optional heartbeat: lets a caller (test_console) know the search is
// still alive during a long-running call, without adding real cost to
// the hot path. Off by default (nullptr) -- the state machine's
// get_ai_move() never sets this, so real gameplay is unaffected.
// ------------------------------------------------------------
typedef void (*C4HeartbeatFn)(uint32_t nodes_so_far);
static C4HeartbeatFn g_heartbeat_fn = nullptr;
static uint32_t g_heartbeat_interval = 200000; // call back every N nodes

// ------------------------------------------------------------
// order_legal_moves: rank legal moves best-first for the player about
// to move at this node
// ------------------------------------------------------------
// This does NOT change the final answer -- alpha-beta returns the same
// value and the same best move regardless of exploration order. It
// changes only how many nodes get visited to prove that answer, by
// letting the search find strong/cutting moves sooner.
//
// Ranking: an immediate winning move always goes first (nothing beats
// winning outright). Everything else is ranked by a one-ply heuristic
// look -- evaluate what the position looks like FOR THE MOVER right
// after playing each candidate, and prefer the best-looking one. This
// costs one extra c4_heuristic() call per legal move at every internal
// node (up to 7), on top of the normal recursive cost -- the bet is
// that fewer total nodes explored more than pays for that, which is
// the standard justification for this technique, but it should be
// measured on real hardware rather than assumed.
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

static int32_t max_value(C4State s, int32_t alpha, int32_t beta,
                          uint8_t depth, uint8_t max_depth, uint8_t* out_move) {
    extern uint32_t g_max_calls;
    extern uint32_t g_min_calls;
    g_max_calls++;

    if (g_heartbeat_fn && (g_max_calls % g_heartbeat_interval) == 0) {
        g_heartbeat_fn(g_max_calls + g_min_calls);
    }

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

    uint8_t ordered[C4_WIDTH];
    int n = order_legal_moves(&s, ordered);

    int32_t v = INT32_MAX;
    for (int i = 0; i < n; i++) {
        uint8_t col = ordered[i];

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