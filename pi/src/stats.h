#pragma once
// Game statistics, same file and format as Finn's connect4.py:
//   ~/connect4_stats.json
//   { "roger": { "noob": {"wins","losses","draws"}, "pro": {...}, "hacker": {...} },
//     "pvp":   { "games", "p1_wins", "p2_wins", "draws" } }
// Roger results are from R.O.G.E.R's point of view.
#include <string>

struct WLD { int wins = 0, losses = 0, draws = 0; int games() const { return wins + losses + draws; } };

struct Stats {
    WLD roger[3];                               // 0 noob, 1 pro, 2 hacker
    int pvpGames = 0, p1 = 0, p2 = 0, pvpDraws = 0;

    static std::string defaultPath();
    bool load(const std::string& path);         // missing/corrupt file -> zeros, returns false
    bool save(const std::string& path) const;   // temp file + rename
    void recordRoger(int difficulty, const char* result);   // "win" / "loss" / "draw"
    void recordPvp(const char* result);                     // "p1" / "p2" / "draw"
    WLD totals() const;
    void clear() { *this = Stats(); }
};
