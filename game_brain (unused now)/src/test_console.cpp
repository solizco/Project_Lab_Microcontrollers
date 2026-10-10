#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include "board.h"
#include "minimax.h"

// ============================================================
// Minimax Test Console
// ============================================================
// Deliberately has NO dependency on display.h, input.h, difficulty.h,
// protocol.h, or Motion Controller -- just board.h + minimax.h + Serial.
// This is a SEPARATE PlatformIO project (its own platformio.ini, no
// Adafruit/Preferences lib_deps needed), meant to sit as a sibling to
// game_brain/ and motion_controller/ rather than replacing either.
// The point is to get real on-target timing for the algorithm alone,
// without waiting on Motion Controller or finished wiring.
//
// Avoids the Arduino String class on purpose -- plain char buffers and
// sscanf() are more predictable on a long-running embedded test (String
// is known to fragment heap over time), and there's no real convenience
// lost for this small a command set.
//
// Commands (type into the Serial Monitor, 115200 baud, newline-terminated):
//   b            - print the current board
//   r            - reset to an empty board
//   m <col>      - apply a move at column 0-6 (alternates player automatically)
//   s <depth>    - run one search at the given depth, print move/value/
//                  nodes/timing
//   sweep <maxd> - run the search at every depth from 1 to maxd, printing
//                  a row per depth (mirrors the Python depth-sweep cell)
//   t0           - load an empty board
//   t1           - load the exact "hard" mid-game position from the real
//                  Hacker-difficulty game that measured ~8.8 million
//                  nodes at depth 10 in the Python test suite -- this is
//                  the position that actually matters: it's what tells
//                  us whether real on-target throughput clears the
//                  10-second budget or not
//   h            - show this help

C4State board;

void print_board() {
    Serial.println("  0 1 2 3 4 5 6");
    for (int r = C4_HEIGHT - 1; r >= 0; r--) { // top row first, visually
        Serial.print("  ");
        for (int c = 0; c < C4_WIDTH; c++) {
            uint64_t bit = 1ULL << (c * 7 + r);
            char ch = '.';
            if (board.bitboard[PLAYER_RED] & bit) ch = 'X';
            else if (board.bitboard[PLAYER_BLACK] & bit) ch = 'O';
            Serial.print(ch);
            Serial.print(' ');
        }
        Serial.println();
    }
    Serial.print("to move: ");
    Serial.println(board.to_move == PLAYER_RED ? "X" : "O");
}

void print_help() {
    Serial.println("\n=== Minimax Test Console ===");
    Serial.println("  b            print board");
    Serial.println("  r            reset to empty board");
    Serial.println("  m N          apply a move at column N (0-6) -- e.g.: m 3");
    Serial.println("  s N          run one search at depth N -- e.g.: s 8");
    Serial.println("  sweep N      run search at every depth 1..N -- e.g.: sweep 10");
    Serial.println("  t0           load empty board");
    Serial.println("  t1           load real ~8.8M-node hard position");
    Serial.println("  h            this help");
    Serial.println("(N is a plain number -- type 's 8', not 's <8>')");
}

unsigned long g_search_start_us; // for the heartbeat callback below

void heartbeat_print(uint32_t nodes_so_far) {
    unsigned long elapsed_ms = (micros() - g_search_start_us) / 1000;
    char line[64];
    snprintf(line, sizeof(line), "  ... still running (%.1fs, %lu nodes so far)",
             elapsed_ms / 1000.0f, (unsigned long)nodes_so_far);
    Serial.println(line);
}

void run_search(uint8_t depth) {
    Serial.print("Searching depth ");
    Serial.print(depth);
    Serial.println(" ...");

    g_heartbeat_fn = heartbeat_print;       // print every 200k nodes so a
    g_heartbeat_interval = 200000;          // long search never looks hung

    g_search_start_us = micros();
    unsigned long start = g_search_start_us;
    SearchResult r = h_alphabeta_search(board, depth);
    unsigned long elapsed_us = micros() - start;

    g_heartbeat_fn = nullptr; // off again once this search is done

    float elapsed_ms = elapsed_us / 1000.0f;
    float nodes_per_sec = (elapsed_us > 0)
        ? (r.nodes * 1000000.0f / elapsed_us)
        : 0.0f;

    char line[128];
    snprintf(line, sizeof(line),
             "  move=%d  value=%ld  nodes=%lu  time_ms=%.2f  nodes/sec=%.0f",
             r.move, (long)r.value, (unsigned long)r.nodes, elapsed_ms, nodes_per_sec);
    Serial.println(line);
}

void load_test_position(int which) {
    c4_init(&board);
    if (which == 1) {
        // Exact reconstruction of the real Hacker-difficulty position
        // from the Python test suite that measured 2,953,766 +
        // 5,813,761 = 8,767,527 nodes at depth 10. Column sequence
        // derived from that game's actual moves (X:0, O:3, X:1, O:3,
        // X:1, O:1, X:0, O:1, X:0 -- alternating from an empty board).
        int seq[] = {0, 3, 1, 3, 1, 1, 0, 1, 0};
        for (int i = 0; i < 9; i++) c4_apply_move(&board, seq[i]);
        Serial.println("Loaded t1: real hard position (~8.8M nodes @ depth 10 in Python)");
    } else {
        Serial.println("Loaded t0: empty board");
    }
}

void handle_command(const char* line) {
    char cmd[16] = {0};
    int arg = -1;
    sscanf(line, "%15s %d", cmd, &arg);

    if (strcmp(cmd, "b") == 0) {
        print_board();
    } else if (strcmp(cmd, "r") == 0) {
        c4_init(&board);
        Serial.println("Board reset.");
    } else if (strcmp(cmd, "m") == 0) {
        if (arg < 0 || arg > 6 || !c4_is_column_legal(&board, arg)) {
            Serial.println("Illegal move.");
        } else {
            c4_apply_move(&board, arg);
            print_board();
        }
    } else if (strcmp(cmd, "s") == 0) {
        if (arg < 1 || arg > 42) {
            Serial.println("depth must be 1-42");
        } else {
            run_search((uint8_t)arg);
        }
    } else if (strcmp(cmd, "sweep") == 0) {
        if (arg < 1 || arg > 42) {
            Serial.println("maxdepth must be 1-42");
        } else {
            for (int d = 1; d <= arg; d++) run_search((uint8_t)d);
        }
    } else if (strcmp(cmd, "t0") == 0) {
        load_test_position(0);
    } else if (strcmp(cmd, "t1") == 0) {
        load_test_position(1);
    } else if (strcmp(cmd, "h") == 0) {
        print_help();
    } else {
        Serial.println("Unknown command. Type 'h' for help.");
    }
}

void setup() {
    Serial.begin(115200);
    c4_init(&board);

    // Proves definitively whether the diagnostic build actually took
    // effect -- no inference from timing needed. If this prints 0 when
    // you expected 1 (or vice versa), the build flag/edit didn't reach
    // the compiler, full stop, regardless of what the .ini says.
    Serial.print("C4_DISABLE_HEURISTIC = ");
    Serial.println(C4_DISABLE_HEURISTIC);

    print_help();

    // Immediate sanity check on power-up, before any typing -- confirms
    // the search runs at all and gives an instant first timing reading.
    Serial.println("\nPower-on sanity check: depth=4 from an empty board");
    run_search(4);
}

void loop() {
    static char buf[64];
    static uint8_t idx = 0;

    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\n' || c == '\r') {
            if (idx > 0) {
                buf[idx] = '\0';
                handle_command(buf);
                idx = 0;
            }
        } else if (c == 0x08 || c == 0x7F) {
            // Backspace/Delete -- remove the last character instead of
            // appending it as data. A terminal sends this as a raw byte,
            // not a "please delete" instruction, so it has to be handled
            // explicitly or it just becomes garbage in the buffer.
            if (idx > 0) idx--;
        } else if (c == 0x1B) {
            // ESC -- the start of an arrow-key/cursor escape sequence
            // (e.g. Left Arrow sends ESC '[' 'D'). This console doesn't
            // support cursor movement or command history, so swallow the
            // ESC and the couple of bytes that typically follow it,
            // rather than letting them get stuffed into the command
            // buffer and corrupt every command typed afterward.
            unsigned long t0 = millis();
            while (Serial.available() < 2 && millis() - t0 < 20) { /* brief wait */ }
            if (Serial.available() >= 1) Serial.read();
            if (Serial.available() >= 1) Serial.read();
        } else if (c >= 0x20 && c < 0x7F && idx < sizeof(buf) - 1) {
            // Only accept printable ASCII into the buffer -- filters out
            // any other stray control byte instead of silently storing
            // it, which is what let one bad keystroke jam the parser
            // until the board was reset.
            buf[idx++] = c;
        } else if (idx >= sizeof(buf) - 1) {
            // Buffer full without ever seeing a newline -- something's
            // wrong (leftover garbage, a bad paste). Discard and start
            // over instead of staying silently stuck.
            idx = 0;
            Serial.println("(line too long or corrupted -- discarded, try again)");
        }
    }
}