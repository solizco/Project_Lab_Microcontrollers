#include "game.h"

static const DifficultyConfig PRESETS[3] = {
    { "Noob",   2, 25, false },
    { "Pro",    8,  0, false },
    { "Hacker", 10, 0, true  },
};

const DifficultyConfig* difficultyFor(uint8_t mode) {
    if (mode < 1 || mode > 3) return nullptr;
    return &PRESETS[mode - 1];
}

SearchResult findBestMove(const Board& b, const DifficultyConfig& cfg,
                          const std::atomic<bool>& cancel, std::mt19937& rng) {
    if (cfg.use_opening_book && b.empty()) {
        SearchResult r; r.move = 3; r.value = 0; r.nodes = 0; r.aborted = false;
        return r;
    }
    SearchResult r = h_alphabeta_search(b.state(), cfg.max_depth, &cancel);
    if (r.aborted) return r;

    // Noob blunder: with blunder_chance_pct probability, play a random legal
    // column other than the best one. r.value is left as the REAL search
    // value so the face reaction still reflects the true position.
    if (cfg.blunder_chance_pct && (int)(rng() % 100) < cfg.blunder_chance_pct) {
        uint8_t alt[C4_WIDTH]; int n = 0;
        for (int c = 0; c < C4_WIDTH; c++)
            if (b.legal(c) && c != r.move) alt[n++] = (uint8_t)c;
        if (n > 0) r.move = alt[rng() % n];
    }
    return r;
}
