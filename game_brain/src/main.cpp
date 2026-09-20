#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "protocol.h"
#include "board.h"
#include "minimax.h"
#include "difficulty.h"
#include "input.h"
#include "display.h"



// ============================================================
// Game Brain: Main State Machine
// ============================================================
// Both the human's button-selected column and the bot's search-
// selected column flow through the SAME two states below
// (AWAITING_DROP handles both identically) -- this mirrors the
// Python test suite's design where difficulty is a config plugged
// into one search function, not a separate code path per difficulty.
// ADJUST these UART pins/baud to match the actual wiring to Motion
// Controller once Guyton's PCB is finalized -- placeholders for now.
#define MOTION_UART_BAUD 115200
#define MOTION_UART_RX   16
#define MOTION_UART_TX   17

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

enum class GameBrainState {
    HOMING,         // sent CMD_HOME, waiting for Motion Controller's CMD_READY
    MENU,           // difficulty selection screen
    HUMAN_TURN,     // waiting for LEFT/RIGHT/CONFIRM to pick a column
    AI_TURN,        // computing the bot's move (currently a placeholder --
                     // see get_ai_move() below)
    AWAITING_DROP,  // CMD_DROP sent, waiting for CMD_DONE or CMD_ERROR
    GAME_OVER,      // showing result, waiting for CONFIRM to play again
    FAULT,          // Motion Controller reported an error -- halted,
                     // requires a physical power cycle to recover
};

GameBrainState state = GameBrainState::HOMING;
ButtonReader buttons;
FrameParser parser;

C4State board;
Difficulty difficulty;
uint8_t menu_selected_index = 1;      // starts on "Pro" -- overwritten
                                        // by load_last_difficulty() in setup()
uint8_t selected_col = 3;             // column selector during HUMAN_TURN,
                                        // starts centered
uint8_t pending_col;                  // column just sent to Motion Controller,
                                        // awaiting confirmation
C4Player pending_mover;               // who made the pending move -- captured
                                        // BEFORE applying it, since to_move
                                        // flips inside c4_apply_move()

// ------------------------------------------------------------
// get_ai_move: real search now wired in
// ------------------------------------------------------------
// Opening book and (future) blunder-chance logic stay OUTSIDE
// h_alphabeta_search, same separation as the Python test suite's
// get_bot_move() -- the search function itself stays pure (always
// returns its best move), and difficulty-flavor decisions (skip
// search on move 1, occasionally play a worse move on Noob) wrap
// around it here.
uint8_t get_ai_move(const C4State* s, const DifficultyConfig* cfg) {
    bool board_empty = (s->bitboard[0] == 0 && s->bitboard[1] == 0);
    if (cfg->use_opening_book && board_empty) {
        return 3; // center column
    }

    SearchResult result = h_alphabeta_search(*s, cfg->max_depth);
    // TODO: Noob's blunder_chance_pct isn't applied yet -- once this
    // is tested and trusted on real hardware, wrap this return with
    // the same random-legal-move substitution logic the Python
    // version used, gated on cfg->blunder_chance_pct.
    return result.move;
}

// ------------------------------------------------------------
// send_drop: Build and transmit a CMD_DROP frame to Motion Controller
// ------------------------------------------------------------
void send_drop(uint8_t col, C4Player mover) {
    uint8_t payload[2] = { col, (uint8_t)mover };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_DROP, payload, 2, frame);
    Serial2.write(frame, len);

    pending_col = col;
    pending_mover = mover;
    state = GameBrainState::AWAITING_DROP;
}

void send_home() {
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_HOME, nullptr, 0, frame);
    Serial2.write(frame, len);
    state = GameBrainState::HOMING;
}

// ------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------
void setup() {
    Serial.begin(115200); // USB serial, for debug prints only

    Serial2.begin(MOTION_UART_BAUD, SERIAL_8N1, MOTION_UART_RX, MOTION_UART_TX);

    tft.init(240, 320); // ADJUST resolution to match the actual 2.0"
                          // panel's real pixel dimensions
    buttons.begin();

    difficulty = load_last_difficulty();
    menu_selected_index = (uint8_t)difficulty;

    c4_init(&board);

    send_home(); // boot handshake: don't trust any prior gantry
                  // position, always home before the first game
}

void loop() {
    // ---- UART receive: poll for a complete frame from Motion Controller ----
    // This runs every loop() iteration regardless of state, since
    // Motion Controller's reply could arrive at any time relative to
    // where the state machine currently is -- but the frame's MEANING
    // is only acted on if we're actually in a state expecting one
    // (HOMING expects READY, AWAITING_DROP expects DONE/ERROR). A
    // frame arriving in an unexpected state is logged and ignored
    // rather than acted on, since acting on it would mean trusting
    // a reply to a command we don't believe we sent.
    while (Serial2.available()) {
        if (parser.feed_byte(Serial2.read())) {
            if (parser.cmd == CMD_READY && state == GameBrainState::HOMING) {
                state = GameBrainState::MENU;
            } else if (parser.cmd == CMD_DONE && state == GameBrainState::AWAITING_DROP) {
                // Sanity-check the echoed column matches what we sent --
                // if it doesn't, something is badly desynced between the
                // two boards, and continuing would risk updating the
                // wrong column's state.
                if (parser.payload[0] != pending_col) {
                    // Mismatch: treat as a fault rather than guessing.
                    state = GameBrainState::FAULT;
                    draw_fault(0xFF); // 0xFF: not a Motion Controller
                                        // error code, but a Game-Brain-
                                        // side desync detection
                } else {
                    c4_apply_move(&board, pending_col);
                    bool won = c4_is_win(board.bitboard[pending_mover]);
                    bool full = c4_is_full(&board);

                    if (won) {
                        int result = (pending_mover == PLAYER_RED) ? +1 : -1;
                        draw_game_over(result);
                        state = GameBrainState::GAME_OVER;
                    } else if (full) {
                        draw_game_over(0);
                        state = GameBrainState::GAME_OVER;
                    } else {
                        state = (pending_mover == PLAYER_RED)
                                    ? GameBrainState::AI_TURN
                                    : GameBrainState::HUMAN_TURN;
                    }
                }
            } else if (parser.cmd == CMD_ERROR && state == GameBrainState::AWAITING_DROP) {
                draw_fault(parser.payload[0]);
                state = GameBrainState::FAULT;
            }
            // Any other (cmd, state) combination: ignore. Notably,
            // a stray CMD_READY while NOT in HOMING, or a stray
            // CMD_DONE/CMD_ERROR while NOT in AWAITING_DROP, should
            // never happen if both sides follow the protocol -- but
            // silently ignoring rather than crashing is the safer
            // failure mode if it ever does.
        }
    }

    Button pressed = buttons.poll();

    switch (state) {
        case GameBrainState::HOMING:
            // Nothing to do here but wait for CMD_READY, handled above.
            break;

        case GameBrainState::MENU:
            if (pressed == Button::LEFT && menu_selected_index > 0) {
                menu_selected_index--;
                draw_menu(menu_selected_index);
            } else if (pressed == Button::RIGHT && menu_selected_index < 2) {
                menu_selected_index++;
                draw_menu(menu_selected_index);
            } else if (pressed == Button::CONFIRM) {
                difficulty = (Difficulty)menu_selected_index;
                save_last_difficulty(difficulty);
                c4_init(&board);
                selected_col = 3;
                draw_board(&board, selected_col);
                state = GameBrainState::HUMAN_TURN; // RED (human) always
                                                       // moves first
            }
            break;

        case GameBrainState::HUMAN_TURN:
            if (pressed == Button::LEFT && selected_col > 0) {
                selected_col--;
                draw_board(&board, selected_col);
            } else if (pressed == Button::RIGHT && selected_col < C4_WIDTH - 1) {
                selected_col++;
                draw_board(&board, selected_col);
            } else if (pressed == Button::CONFIRM) {
                if (c4_is_column_legal(&board, selected_col)) {
                    send_drop(selected_col, PLAYER_RED);
                    draw_board(&board, 0xFF); // clear the selector
                                                // indicator while we wait
                }
                // else: column is full, ignore the press -- could add
                // a brief flash/beep here as feedback that the move
                // was rejected, once the actual buzzer/LED hardware
                // is decided
            }
            break;

        case GameBrainState::AI_TURN: {
            uint8_t col = get_ai_move(&board, &DIFFICULTY_PRESETS[difficulty]);
            send_drop(col, PLAYER_BLACK);
            break;
        }

        case GameBrainState::AWAITING_DROP:
            // Nothing to do here but wait for CMD_DONE/CMD_ERROR,
            // handled in the UART receive block above.
            break;

        case GameBrainState::GAME_OVER:
            if (pressed == Button::CONFIRM) {
                c4_init(&board);
                send_home(); // re-home before the next game, same as boot
            }
            break;

        case GameBrainState::FAULT:
            // Deliberately no button handling here -- see display.h's
            // draw_fault() comment. Recovery is a manual power cycle,
            // not a software retry, since the mechanism itself needs
            // a human to check on it.
            break;
    }
}
