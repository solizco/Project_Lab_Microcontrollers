// AI port correctness tests. Run on the laptop (and again on the Pi).
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include "../src/ai.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("  FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static C4State play(std::initializer_list<int> cols) {
    C4State s; c4_init(&s);
    for (int c : cols) { if (!c4_is_column_legal(&s, c)) { printf("bad test setup\n"); exit(2);} c4_apply_move(&s, c); }
    return s;
}

// ---- independent reference: plain minimax, no pruning, no ordering ----
static C4Player g_ref_player;
static int32_t ref_value(const C4State& s, int depth, int max_depth) {
    C4Player last = (s.to_move == PLAYER_RED) ? PLAYER_BLACK : PLAYER_RED;
    if (c4_is_win(s.bitboard[last]))
        return (last == g_ref_player ? 1 : -1) * (int32_t)(C4_TERMINAL_BIG - depth);
    if (c4_is_full(&s)) return 0;
    if (depth >= max_depth) return ai_heuristic(&s, g_ref_player);
    bool maxing = (s.to_move == g_ref_player);
    int32_t best = maxing ? INT32_MIN : INT32_MAX;
    for (int c = 0; c < C4_WIDTH; c++) {
        if (!c4_is_column_legal(&s, c)) continue;
        C4State n = s; c4_apply_move(&n, c);
        int32_t v = ref_value(n, depth + 1, max_depth);
        best = maxing ? std::max(best, v) : std::min(best, v);
    }
    return best;
}

static void test_board_wins() {
    printf("board win detection\n");
    // vertical
    CHECK(c4_is_win(play({0,1,0,1,0,1,0}).bitboard[PLAYER_RED]), "vertical");
    // horizontal
    CHECK(c4_is_win(play({0,0,1,1,2,2,3}).bitboard[PLAYER_RED]), "horizontal");
    // diagonal /
    CHECK(c4_is_win(play({0,1,1,2,2,3,2,3,3,6,3}).bitboard[PLAYER_RED]), "diag /");
    // diagonal \ (mirror)
    CHECK(c4_is_win(play({6,5,5,4,4,3,4,3,3,0,3}).bitboard[PLAYER_RED]), "diag \\");
    // wraparound: top of col 0..., three in a row spanning column edge must NOT win
    C4State s; c4_init(&s);
    for (int c : {0,0,0,0,0,0}) c4_apply_move(&s, c);          // fill col 0
    for (int c : {1,1,1,1,1,1}) c4_apply_move(&s, c);          // fill col 1
    CHECK(!c4_is_win(s.bitboard[PLAYER_RED]) && !c4_is_win(s.bitboard[PLAYER_BLACK]), "no phantom win on filled columns");
}

static void test_tactics() {
    printf("tactics\n");
    // RED has 3 on the bottom row (0,1,2), BLACK scattered: RED to move must play 3
    C4State s = play({0,0,1,1,2,2});
    SearchResult r = h_alphabeta_search(s, 4);
    CHECK(!r.aborted && r.move == 3, "immediate win, got %d", r.move);
    CHECK(r.value >= C4_TERMINAL_BIG - 10, "win value %d", r.value);

    // BLACK threatens bottom row 0,1,2 -> RED (to move) must block at 3.
    // R: 6,6,5 (no threats), B: 0,1,2.
    s = play({6,0,6,1,5,2});
    r = h_alphabeta_search(s, 4);
    CHECK(r.move == 3, "must block at 3, got %d", r.move);

    // Prefer the FASTER win: RED can win now at 3, or later; must pick now.
    s = play({0,0,1,1,2,2});
    r = h_alphabeta_search(s, 8);
    CHECK(r.move == 3 && r.value == C4_TERMINAL_BIG - 1, "faster win, move %d value %d", r.move, r.value);
}

static void test_matches_reference() {
    printf("alpha-beta value == plain minimax on random positions\n");
    std::mt19937 rng(12345);
    int checked = 0;
    for (int trial = 0; trial < 120; trial++) {
        C4State s; c4_init(&s);
        int plies = rng() % 22;
        bool ok = true;
        for (int i = 0; i < plies; i++) {
            C4Player last = s.to_move;
            int legal[7], n = 0;
            for (int c = 0; c < 7; c++) if (c4_is_column_legal(&s, c)) legal[n++] = c;
            if (!n) { ok = false; break; }
            c4_apply_move(&s, legal[rng() % n]);
            if (c4_is_win(s.bitboard[last]) || c4_is_full(&s)) { ok = false; break; }
        }
        if (!ok) continue;
        int depth = 1 + (int)(rng() % 5);
        g_ref_player = s.to_move;
        int32_t expect = ref_value(s, 0, depth);
        SearchResult r = h_alphabeta_search(s, depth);
        CHECK(r.value == expect, "trial %d depth %d plies %d: ab=%d ref=%d", trial, depth, plies, r.value, expect);
        // the chosen move must itself achieve that value
        C4State c = s; CHECK(c4_is_column_legal(&c, r.move), "illegal move %d", r.move);
        c4_apply_move(&c, r.move);
        CHECK(ref_value(c, 1, depth) == expect, "chosen move %d does not achieve value", r.move);
        checked++;
    }
    CHECK(checked > 60, "only %d positions checked", checked);
    printf("  %d positions compared\n", checked);
}

static void test_cancel() {
    printf("cancel flag\n");
    C4State s = play({3,3,2,4});
    using clk = std::chrono::steady_clock;

    // 1. Pre-set cancel: aborts immediately, no move returned.
    std::atomic<bool> pre(true);
    auto t0 = clk::now();
    SearchResult r = h_alphabeta_search(s, 12, &pre);
    double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    CHECK(r.aborted && r.move == 0xFF, "pre-set cancel: aborted=%d move=%d", r.aborted, r.move);
    CHECK(ms < 100, "pre-set cancel took %.1f ms", ms);

    // 2. Cancel a deep search mid-flight from another thread.
    std::atomic<bool> cancel(false);
    SearchResult deep{};
    t0 = clk::now();
    std::thread worker([&] { deep = h_alphabeta_search(s, 14, &cancel); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto c0 = clk::now();
    cancel = true;
    worker.join();
    double unwind = std::chrono::duration<double, std::milli>(clk::now() - c0).count();
    CHECK(deep.aborted && deep.move == 0xFF, "mid-flight: aborted=%d move=%d", deep.aborted, deep.move);
    CHECK(unwind < 150, "unwind took %.1f ms", unwind);
    printf("  cancel -> returned in %.2f ms (nodes searched %u)\n", unwind, deep.nodes);

    // 3. A search after an aborted one is unaffected (no leaked state).
    std::atomic<bool> no(false);
    SearchResult a = h_alphabeta_search(s, 6);
    SearchResult b = h_alphabeta_search(s, 6, &no);
    CHECK(!a.aborted && !b.aborted, "stale abort state");
    CHECK(a.move == b.move && a.value == b.value && a.nodes == b.nodes,
          "flag pointer changed result: %d/%d/%u vs %d/%d/%u", a.move, a.value, a.nodes, b.move, b.value, b.nodes);
}

int main() {
    test_board_wins();
    test_tactics();
    test_matches_reference();
    test_cancel();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
