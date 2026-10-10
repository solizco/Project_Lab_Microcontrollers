#pragma once
#include <Preferences.h>  // Arduino-ESP32's wrapper around NVS flash storage

// ============================================================
// difficulty.h -- Noob / Pro / Hacker Presets + Persisted Selection
// ============================================================
// Mirrors the DifficultyConfig design from the Python test suite:
// difficulty is a DATA config plugged into one search function, not a
// different algorithm per tier. max_depth here is a placeholder until
// the bitboard minimax port existsit's wired through now so the menu
// and state machine have something real to select between, and the
// AI turn handler just needs its stub swapped for the real search
// later without any other code changing.

typedef enum { DIFF_NOOB = 0, DIFF_PRO = 1, DIFF_HACKER = 2 } Difficulty;

typedef struct {
    const char* name;
    uint8_t max_depth;
    uint8_t blunder_chance_pct; // 0-100
    bool use_opening_book;
} DifficultyConfig;

static const DifficultyConfig DIFFICULTY_PRESETS[3] = {
    { "Noob",   2,  25, false },
    { "Pro",    8,  0,  false },
    { "Hacker", 10, 0,  true  },
};

// ------------------------------------------------------------
// NVS persistence: remember the last difficulty across power cycles
// ------------------------------------------------------------
// Preferences opens a named namespace in flash -- "c4game" here just
// needs to be unique to this app so it doesn't collide with any other
// stored settings on the same chip.

inline Difficulty load_last_difficulty() {
    Preferences prefs;
    prefs.begin("c4game", /*readOnly=*/true);
    // getUChar's second argument is the default returned if the key
    // has never been written -- DIFF_PRO on first-ever boot, a
    // reasonable middle default rather than defaulting to the
    // hardest or easiest option.
    uint8_t stored = prefs.getUChar("difficulty", DIFF_PRO);
    prefs.end();
    if (stored > DIFF_HACKER) stored = DIFF_PRO; // guard against
                                                    // corrupted/unexpected
                                                    // flash contents
    return (Difficulty)stored;
}

inline void save_last_difficulty(Difficulty d) {
    Preferences prefs;
    prefs.begin("c4game", /*readOnly=*/false);
    prefs.putUChar("difficulty", (uint8_t)d);
    prefs.end();
}
