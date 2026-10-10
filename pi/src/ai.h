#pragma once
#include <atomic>
#include <stdint.h>
#include "board.h"

// Terminal outcomes score +/-(C4_TERMINAL_BIG - depth). Heuristic scores
// stay in the low hundreds, so the two ranges never overlap.
#define C4_TERMINAL_BIG 1000000

typedef struct {
    uint8_t  move;     // chosen column, or 0xFF if the search was aborted
    int32_t  value;    // score from the root mover's point of view (0 if aborted)
    uint32_t nodes;    // nodes visited
    bool     aborted;  // true = cancel flag fired; move/value are NOT valid
} SearchResult;

// Alpha-beta search from `root`, `max_depth` plies. If `cancel` is non-null
// it is polled every 1024 nodes; once it reads true the search unwinds and
// returns aborted=true. Only one search may run at a time (file-static
// state). Safe to call from a worker thread.
SearchResult h_alphabeta_search(C4State root, uint8_t max_depth,
                                const std::atomic<bool>* cancel = nullptr);

// Exposed for tests only.
int32_t ai_heuristic(const C4State* s, C4Player player);
