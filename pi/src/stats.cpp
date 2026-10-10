#include "stats.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include "json_lite.h"

static const char* DIFF[3] = { "noob", "pro", "hacker" };

std::string Stats::defaultPath() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/connect4_stats.json";
}

bool Stats::load(const std::string& path) {
    *this = Stats();
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    JVal v;
    if (!JParser(ss.str()).parse(v) || v.type != JVal::OBJ) return false;
    auto num = [](const JVal* o, const char* k) { const JVal* x = o ? o->get(k) : nullptr; return x ? (int)x->numOr(0) : 0; };
    const JVal* r = v.get("roger");
    for (int d = 0; d < 3; d++) {
        const JVal* o = r ? r->get(DIFF[d]) : nullptr;
        roger[d].wins = num(o, "wins"); roger[d].losses = num(o, "losses"); roger[d].draws = num(o, "draws");
    }
    const JVal* p = v.get("pvp");
    pvpGames = num(p, "games"); p1 = num(p, "p1_wins"); p2 = num(p, "p2_wins"); pvpDraws = num(p, "draws");
    return true;
}

bool Stats::save(const std::string& path) const {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    fprintf(f, "{\n  \"roger\": {\n");
    for (int d = 0; d < 3; d++)
        fprintf(f, "    \"%s\": {\n      \"wins\": %d,\n      \"losses\": %d,\n      \"draws\": %d\n    }%s\n",
                DIFF[d], roger[d].wins, roger[d].losses, roger[d].draws, d < 2 ? "," : "");
    fprintf(f, "  },\n  \"pvp\": {\n    \"games\": %d,\n    \"p1_wins\": %d,\n    \"p2_wins\": %d,\n    \"draws\": %d\n  }\n}\n",
            pvpGames, p1, p2, pvpDraws);
    bool ok = fflush(f) == 0;
    fclose(f);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

void Stats::recordRoger(int d, const char* result) {
    if (d < 0 || d > 2) return;
    if (!strcmp(result, "win")) roger[d].wins++;
    else if (!strcmp(result, "loss")) roger[d].losses++;
    else roger[d].draws++;
}

void Stats::recordPvp(const char* result) {
    pvpGames++;
    if (!strcmp(result, "p1")) p1++;
    else if (!strcmp(result, "p2")) p2++;
    else pvpDraws++;
}

WLD Stats::totals() const {
    WLD t;
    for (const auto& r : roger) { t.wins += r.wins; t.losses += r.losses; t.draws += r.draws; }
    return t;
}
