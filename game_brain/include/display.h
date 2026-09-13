#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "board.h"
#include "difficulty.h"

// ============================================================
// display.h -- ST7789 Rendering
// ============================================================
// IMPORTANT: the layouts below (piece size, spacing, colors, text
// position) are placeholder/reference values, not a finished design --
// they're here so the state machine has real functions to call and
// something visible to test against, but expect to tune the numbers
// once the actual 2.0" panel is in hand and you can see it rendered.
//
// ADJUST these pin numbers to match the actual SPI wiring once
// Guyton's PCB is finalized.
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4

// Colors (RGB565 format, which is what Adafruit_GFX expects)
#define COLOR_BG     0x0000  // black
#define COLOR_GRID   0x001F  // blue
#define COLOR_RED    0xF800
#define COLOR_BLACK  0x39E7  // dark gray (so black pieces are visible
                               // against the black background, rather
                               // than invisible)
#define COLOR_WHITE  0xFFFF
#define COLOR_HILITE 0xFFE0  // yellow, for the column-select indicator
                               // and the currently-highlighted menu item

extern Adafruit_ST7789 tft; // instantiated once in main.cpp

// ------------------------------------------------------------
// draw_menu: Difficulty selection screen
// ------------------------------------------------------------
// selected_index: which of the 3 DIFFICULTY_PRESETS is currently
// highlighted (before CONFIRM locks it in).
inline void draw_menu(uint8_t selected_index) {
    tft.fillScreen(COLOR_BG);
    tft.setTextSize(2);

    tft.setCursor(20, 20);
    tft.setTextColor(COLOR_WHITE);
    tft.print("Select Difficulty");

    for (int i = 0; i < 3; i++) {
        int y = 60 + i * 40;
        tft.setCursor(30, y);
        if (i == selected_index) {
            tft.setTextColor(COLOR_HILITE);
            tft.print("> ");
        } else {
            tft.setTextColor(COLOR_WHITE);
            tft.print("  ");
        }
        tft.print(DIFFICULTY_PRESETS[i].name);
    }
}

// ------------------------------------------------------------
// draw_board: The Connect Four grid + current pieces
// ------------------------------------------------------------
// highlighted_col: during a human turn, which column the LEFT/RIGHT
// selector currently points at (draws an indicator above that column).
// Pass 0xFF for "no highlight" (e.g. during the AI's turn, or once a
// move has been sent and we're just waiting on Motion Controller).
inline void draw_board(const C4State* s, uint8_t highlighted_col) {
    tft.fillScreen(COLOR_BG);

    const int cell_size = 30;      // pixels per cell -- TUNE to fit
                                     // the actual 2.0" panel resolution
    const int board_x = 10;         // top-left origin of the grid
    const int board_y = 30;

    // Column-select indicator: a small triangle/marker above the
    // highlighted column, only drawn during human input.
    if (highlighted_col != 0xFF) {
        int cx = board_x + highlighted_col * cell_size + cell_size / 2;
        tft.fillTriangle(cx - 6, board_y - 8, cx + 6, board_y - 8,
                          cx, board_y - 2, COLOR_HILITE);
    }

    for (int c = 0; c < C4_WIDTH; c++) {
        for (int r = 0; r < C4_HEIGHT; r++) {
            int px = board_x + c * cell_size;
            // Flip row for drawing: row 0 is the BOTTOM of the board
            // in game logic, but screen y increases DOWNWARD, so the
            // bottom game row needs to draw at the highest y value.
            int py = board_y + (C4_HEIGHT - 1 - r) * cell_size;

            uint64_t bit = 1ULL << (c * 7 + r);
            uint16_t color = COLOR_GRID; // empty cell outline color
            bool filled = false;

            if (s->bitboard[PLAYER_RED] & bit) {
                color = COLOR_RED;
                filled = true;
            } else if (s->bitboard[PLAYER_BLACK] & bit) {
                color = COLOR_BLACK;
                filled = true;
            }

            int center_x = px + cell_size / 2;
            int center_y = py + cell_size / 2;
            int radius = cell_size / 2 - 3;

            if (filled) {
                tft.fillCircle(center_x, center_y, radius, color);
            } else {
                tft.drawCircle(center_x, center_y, radius, COLOR_GRID);
            }
        }
    }
}

// ------------------------------------------------------------
// draw_game_over: Win/loss/draw result screen
// ------------------------------------------------------------
// result: +1 = human (RED) won, -1 = bot (BLACK) won, 0 = draw --
// matching the Python test suite's utility() convention.
inline void draw_game_over(int result) {
    tft.fillScreen(COLOR_BG);
    tft.setTextSize(2);
    tft.setCursor(30, 80);
    tft.setTextColor(COLOR_WHITE);

    if (result > 0) {
        tft.setTextColor(COLOR_RED);
        tft.print("You win!");
    } else if (result < 0) {
        tft.setTextColor(COLOR_BLACK);
        tft.print("Computer wins!");
    } else {
        tft.print("Draw!");
    }

    tft.setTextSize(1);
    tft.setCursor(20, 140);
    tft.setTextColor(COLOR_WHITE);
    tft.print("Press CONFIRM to play again");
}

// ------------------------------------------------------------
// draw_fault: Motion Controller reported an error -- halt and show it
// ------------------------------------------------------------
// This screen is deliberately blunt (red background) since it means
// the physical mechanism needs human attention -- a jam, a stall,
// something the software can't recover from on its own. The game
// should NOT continue past this state automatically.
inline void draw_fault(uint8_t error_code) {
    tft.fillScreen(COLOR_RED);
    tft.setTextSize(2);
    tft.setTextColor(COLOR_WHITE);
    tft.setCursor(10, 60);
    tft.print("MOTION FAULT");
    tft.setTextSize(1);
    tft.setCursor(10, 100);
    tft.print("Error code: ");
    tft.print(error_code);
    tft.setCursor(10, 120);
    tft.print("Check mechanism, then");
    tft.setCursor(10, 135);
    tft.print("power-cycle to reset.");
}
