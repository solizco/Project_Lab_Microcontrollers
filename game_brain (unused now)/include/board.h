#pragma once
#include <stdint.h>

// ============================================================
// board.h -- Bitboard Connect Four Representation
// ============================================================
// Two separate 64-bit bitboards, one per player, replace the Python
// test suite's dict-based Board. This is the representation the
// minimax/alpha-beta port will eventually search over -- building it
// correctly now means the AI port later just plugs in, rather than
// needing its own board rewrite.
//
// ENCODING: 7 bits per column, 7 columns = 49 bits total (fits easily
// in a uint64_t). Within a column, rows 0-5 are the 6 real playable
// rows; row 6 is an unused "guard" bit that is NEVER set by real game
// play. bit index = column*7 + row.
//
// The guard row exists purely so the bit-shift win-detection trick
// below can't accidentally read a "4 in a row" that actually spans
// across a column boundary (e.g. row 5 of column 2 lining up with row
// 0 of column 3 during a horizontal shift). As long as game code never
// sets a guard bit, this can't happen -- see is_column_legal() below,
// which caps every column at row 5.
//
// This win-detection algorithm and 7-bit-per-column/guard-row layout
// is a well-established public technique for Connect Four bitboards
// (used in fast solvers); it has been compile-tested here against
// vertical, horizontal, both diagonal, and wraparound-edge cases
// before being included in this header.

#define C4_WIDTH   7
#define C4_HEIGHT  6

typedef enum { PLAYER_RED = 0, PLAYER_BLACK = 1 } C4Player;

typedef struct {
    uint64_t bitboard[2];       // bitboard[PLAYER_RED], bitboard[PLAYER_BLACK]
    uint8_t  heights[C4_WIDTH]; // heights[c] = next free row in column c (0-5
                                 // means still room; 6 means the column is full)
    C4Player to_move;
} C4State;

// ------------------------------------------------------------
// c4_init: Set up an empty starting board
// ------------------------------------------------------------
inline void c4_init(C4State* s) {
    s->bitboard[0] = 0;
    s->bitboard[1] = 0;
    for (int c = 0; c < C4_WIDTH; c++) s->heights[c] = 0;
    s->to_move = PLAYER_RED; // RED always moves first, matching the
                               // Python test suite's 'X' convention
}

// ------------------------------------------------------------
// c4_is_column_legal: Can a piece still be dropped in this column?
// ------------------------------------------------------------
inline bool c4_is_column_legal(const C4State* s, uint8_t col) {
    if (col >= C4_WIDTH) return false;
    return s->heights[col] < C4_HEIGHT; // heights[col]==6 means full
                                          // (row 6 is the guard, never
                                          // a legal landing row)
}

// ------------------------------------------------------------
// c4_apply_move: Drop the current player's piece into `col`
// ------------------------------------------------------------
// Caller MUST check c4_is_column_legal() first -- this function does
// not itself validate and will corrupt board state (by writing into
// or past the guard bit) if called on a full column. Keeping the
// check separate mirrors how Game Brain will use it: legality is
// checked once, up front, before ever calling apply.
inline void c4_apply_move(C4State* s, uint8_t col) {
    uint8_t row = s->heights[col];
    uint64_t bit = 1ULL << (col * 7 + row);
    s->bitboard[s->to_move] |= bit;
    s->heights[col] = row + 1;
    s->to_move = (s->to_move == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
}

// ------------------------------------------------------------
// c4_is_win: Does this specific player's bitboard contain 4-in-a-row?
// ------------------------------------------------------------
// Only ever needs to be checked for whichever player just moved --
// a move can only ever complete a win for the player who made it, so
// Game Brain should call this once per move, on the mover's bitboard,
// rather than checking both players every turn.
inline bool c4_is_win(uint64_t bb) {
    uint64_t m;

    m = bb & (bb >> 1);        // vertical (step 1: adjacent row, same column)
    if (m & (m >> 2)) return true;

    m = bb & (bb >> 7);        // horizontal (step 7: same row, next column)
    if (m & (m >> 14)) return true;

    m = bb & (bb >> 8);        // diagonal "/" (step 8 = 7+1: up-right)
    if (m & (m >> 16)) return true;

    m = bb & (bb >> 6);        // diagonal "\" (step 6 = 7-1: down-right)
    if (m & (m >> 12)) return true;

    return false;
}

// ------------------------------------------------------------
// c4_is_full: Has every column been filled (draw, if nobody has won)?
// ------------------------------------------------------------
inline bool c4_is_full(const C4State* s) {
    for (int c = 0; c < C4_WIDTH; c++) {
        if (s->heights[c] < C4_HEIGHT) return false;
    }
    return true;
}
