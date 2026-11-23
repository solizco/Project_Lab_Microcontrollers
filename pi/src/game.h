#pragma once
// ============================================================
// game.h -- the interface the Pi game code talks to
// ============================================================
// Thin layer over board.h / ai.h so the UI and state machine never touch
// bitboards. Colors: 0 = RED, 1 = BLACK (same as the wire protocol).
#include <atomic>
#include <random>
#include <stdint.h>
#include "ai.h"
#include "board.h"

class Board {
public:
    Board() { reset(); }
    // first: 0 = RED moves first (R.O.G.E.R / Player One), 1 = BLACK (the human / Player Two)
    void reset(int first = 0)       { c4_init(&s_); s_.to_move = first ? PLAYER_BLACK : PLAYER_RED; }
    bool legal(int col) const       { return col >= 0 && col < C4_WIDTH && c4_is_column_legal(&s_, (uint8_t)col); }
    void play(int col)              { c4_apply_move(&s_, (uint8_t)col); }   // caller checks legal() first
    bool won(int player) const      { return c4_is_win(s_.bitboard[player]); }
    bool full() const               { return c4_is_full(&s_); }
    // -1 = empty, 0 = RED, 1 = BLACK. row 0 is the BOTTOM row.
    int  at(int col, int row) const {
        uint64_t bit = 1ULL << (col * 7 + row);
        if (s_.bitboard[PLAYER_RED] & bit)   return 0;
        if (s_.bitboard[PLAYER_BLACK] & bit) return 1;
        return -1;
    }
    int  toMove() const             { return (int)s_.to_move; }
    bool empty() const              { return s_.bitboard[0] == 0 && s_.bitboard[1] == 0; }
    const C4State& state() const    { return s_; }   // copied into the search thread
private:
    C4State s_;
};

struct DifficultyConfig {
    const char* name;
    uint8_t max_depth;
    uint8_t blunder_chance_pct;   // Noob only: chance to play a random non-best move
    bool    use_opening_book;     // Hacker only: center column on an empty board
};

// Mode numbering matches protocol.h SelectedMode: 0 Pass&Play, 1 Noob, 2 Pro, 3 Hacker.
// Returns nullptr for Pass&Play (no AI) or an invalid mode.
const DifficultyConfig* difficultyFor(uint8_t mode);

// Runs the opening book / search / Noob blunder. If `cancel` goes true the
// returned result has aborted=true and must be discarded.
SearchResult findBestMove(const Board& b, const DifficultyConfig& cfg,
                          const std::atomic<bool>& cancel, std::mt19937& rng);
