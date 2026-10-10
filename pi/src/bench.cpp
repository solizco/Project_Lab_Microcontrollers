// Search speed benchmark. RUN THIS FIRST ON THE PI ZERO.
// Positions match the ones timed on the ESP32 so the numbers compare directly.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "ai.h"

static void run(const char* name, const C4State& s, int depth) {
    auto t0 = std::chrono::steady_clock::now();
    SearchResult r = h_alphabeta_search(s, (uint8_t)depth);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("%-28s depth %2d  move=%d value=%-8d nodes=%-9u %9.1f ms  %8.0f nodes/s\n",
           name, depth, r.move, r.value, r.nodes, ms, ms > 0 ? r.nodes * 1000.0 / ms : 0.0);
    fflush(stdout);
}

int main(int argc, char** argv) {
    int maxd = (argc > 1) ? atoi(argv[1]) : 10;
    C4State hard; c4_init(&hard);
    int seq[] = {0, 3, 1, 3, 1, 1, 0, 1, 0};     // the "hard" position (ESP32: ~2.4 s at depth 10)
    for (int c : seq) c4_apply_move(&hard, c);
    C4State empty; c4_init(&empty);
    printf("--- empty board (Pro = depth 8) ---\n");
    for (int d : {4, 6, 8}) if (d <= maxd) run("empty", empty, d);
    printf("--- hard mid-game position (Hacker = depth 10) ---\n");
    for (int d : {6, 8, 10}) if (d <= maxd) run("hard", hard, d);
    return 0;
}
