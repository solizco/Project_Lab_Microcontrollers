#include <Arduino.h>
#include "protocol.h"
#include "board.h"
#include "minimax.h"
#include "difficulty.h"
#include "input.h"

// ============================================================
// Game Brain: Main State Machine
// ============================================================
// Game Brain owns NO display of its own -- the single shared screen
// lives entirely on Motion Controller, which switches between two
// mutually exclusive modes:
//   MENU mode:  active from boot/homing through difficulty selection
//   Roger mode: takes over once gameplay starts (idle while waiting
//               for the human, with a column-selector indicator
//               alongside it; thinking during AI's turn; good/bad
//               move reactions; win/loss at game end) and stays
//               active for the whole game
// Every visual cue Game Brain wants shown is a fire-and-forget UART
// command (CMD_MENU, CMD_SELECTOR, CMD_FACE, CMD_FAULT) -- Game Brain
// never waits on or checks for a reply to any of them, so a missed
// display cue can never stall the game.
//
// Both the human's button-selected column and the bot's search-
// selected column flow through the SAME two states below
// (AWAITING_DROP handles both identically) -- this mirrors the
// Python test suite's design where difficulty is a config plugged
// into one search function, not a separate code path per difficulty.
//
// ADJUST these UART pins/baud to match the actual wiring to Motion
// Controller once Guyton's PCB is finalized -- placeholders for now.
#define MOTION_UART_BAUD 115200
#define MOTION_UART_RX   16
#define MOTION_UART_TX   17

// TEMPORARY: skip the difficulty menu entirely so we can validate the
// core AI <-> Motion Controller loop first (human picks a column, bot
// searches and picks its own, both flow through the same drop/confirm
// path) without menu/display work in the way. Boots straight into a
// Pro-difficulty game the instant homing completes. Set to 0 once this
// integration is confirmed working, to restore the real menu flow.
#define SKIP_MENU_FOR_TESTING 1

enum class GameBrainState {
    HOMING,         // sent CMD_HOME, waiting for Motion Controller's CMD_READY
    MENU,           // difficulty selection screen
    HUMAN_TURN,     // waiting for LEFT/RIGHT/CONFIRM to pick a column
    AI_TURN,        // computing the bot's move
    AWAITING_DROP,  // CMD_DROP sent, waiting for CMD_DONE or CMD_ERROR
    GAME_OVER,      // showing result, waiting for CONFIRM to play again
    FAULT,          // a fault occurred -- halted, requires a physical
                     // power cycle to recover
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
SearchResult get_ai_move(const C4State* s, const DifficultyConfig* cfg) {
    bool board_empty = (s->bitboard[0] == 0 && s->bitboard[1] == 0);
    if (cfg->use_opening_book && board_empty) {
        SearchResult r;
        r.move = 3;
        r.value = 0;
        r.nodes = 0;
        return r;
    }
    SearchResult result = h_alphabeta_search(*s, cfg->max_depth);
    // TODO: Noob's blunder_chance_pct isn't applied yet.
    return result;
}

// ------------------------------------------------------------
// Fire-and-forget display commands to Motion Controller
// ------------------------------------------------------------
void send_face(FaceEvent evt) {
    uint8_t payload[1] = { (uint8_t)evt };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_FACE, payload, 1, frame);
    Serial2.write(frame, len);
}

void send_menu(uint8_t selected_index) {
    uint8_t payload[1] = { selected_index };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_MENU, payload, 1, frame);
    Serial2.write(frame, len);
}

void send_selector(uint8_t col) {
    uint8_t payload[1] = { col };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_SELECTOR, payload, 1, frame);
    Serial2.write(frame, len);
}

void send_fault(uint8_t error_code) {
    uint8_t payload[1] = { error_code };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_FAULT, payload, 1, frame);
    Serial2.write(frame, len);
}

// ------------------------------------------------------------
// classify_and_send_reaction
// ------------------------------------------------------------
FaceEvent last_reaction_face = FACE_IDLE;

void classify_and_send_reaction(const SearchResult& result) {
    bool proven_win_for_ai = (result.value >= (int32_t)(1000000 - 42));

    FaceEvent candidate = FACE_IDLE;
    uint8_t trigger_chance = 0;

    if (proven_win_for_ai) {
        candidate = FACE_BAD_MOVE;
        trigger_chance = 70;
    } else if (result.value > 40) {
        candidate = FACE_BAD_MOVE;
        trigger_chance = 45;
    } else if (result.value < -10) {
        candidate = FACE_GOOD_MOVE;
        trigger_chance = 45;
    }

    if (candidate == FACE_IDLE) {
        last_reaction_face = FACE_IDLE;
        return;
    }

    if (candidate == last_reaction_face) {
        trigger_chance /= 2;
    }

    if ((uint8_t)random(100) < trigger_chance) {
        send_face(candidate);
        last_reaction_face = candidate;
    }
}

// ------------------------------------------------------------
// send_drop / send_home
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
    Serial.begin(115200);
    Serial2.begin(MOTION_UART_BAUD, SERIAL_8N1, MOTION_UART_RX, MOTION_UART_TX);

    buttons.begin();

    difficulty = load_last_difficulty();
    menu_selected_index = (uint8_t)difficulty;

    c4_init(&board);

    send_home();
    Serial.println("Game Brain booted. Sent CMD_HOME, waiting for CMD_READY...");
}

void loop() {
    while (Serial2.available()) {
        if (parser.feed_byte(Serial2.read())) {
            if (parser.cmd == CMD_READY && state == GameBrainState::HOMING) {
#if SKIP_MENU_FOR_TESTING
                difficulty = DIFF_PRO;
                c4_init(&board);
                selected_col = 3;
                state = GameBrainState::HUMAN_TURN;
                send_face(FACE_IDLE);
                send_selector(selected_col);
                Serial.println("Received CMD_READY -- SKIP_MENU_FOR_TESTING: starting Pro game directly");
#else
                state = GameBrainState::MENU;
                send_menu(menu_selected_index);
                Serial.println("Received CMD_READY -- entering MENU");
#endif
            } else if (parser.cmd == CMD_DONE && state == GameBrainState::AWAITING_DROP) {
                if (parser.payload[0] != pending_col) {
                    state = GameBrainState::FAULT;
                    send_fault(0xFF);
                    Serial.println("FAULT: CMD_DONE column mismatch -- boards desynced");
                } else {
                    Serial.print("Received CMD_DONE for column ");
                    Serial.println(pending_col);

                    c4_apply_move(&board, pending_col);
                    bool won = c4_is_win(board.bitboard[pending_mover]);
                    bool full = c4_is_full(&board);

                    if (won) {
                        send_face(pending_mover == PLAYER_RED ? FACE_LOSS : FACE_WIN);
                        state = GameBrainState::GAME_OVER;
                        Serial.println(pending_mover == PLAYER_RED ? "Human WINS!" : "AI WINS!");
                    } else if (full) {
                        send_face(FACE_IDLE);
                        state = GameBrainState::GAME_OVER;
                        Serial.println("Board full -- DRAW");
                    } else {
                        state = (pending_mover == PLAYER_RED)
                                    ? GameBrainState::AI_TURN
                                    : GameBrainState::HUMAN_TURN;
                        Serial.println(state == GameBrainState::AI_TURN
                                            ? "-> AI_TURN" : "-> HUMAN_TURN");
                        if (state == GameBrainState::AI_TURN) {
                            send_face(FACE_THINKING);
                        } else {
                            send_face(FACE_IDLE);
                            send_selector(selected_col);
                        }
                    }
                }
            } else if (parser.cmd == CMD_ERROR && state == GameBrainState::AWAITING_DROP) {
                send_fault(parser.payload[0]);
                state = GameBrainState::FAULT;
                Serial.print("FAULT: Motion Controller reported error code ");
                Serial.println(parser.payload[0]);
            }
        }
    }

    Button pressed = buttons.poll();

    switch (state) {
        case GameBrainState::HOMING:
            break;

        case GameBrainState::MENU:
            if (pressed == Button::LEFT && menu_selected_index > 0) {
                menu_selected_index--;
                send_menu(menu_selected_index);
            } else if (pressed == Button::RIGHT && menu_selected_index < 2) {
                menu_selected_index++;
                send_menu(menu_selected_index);
            } else if (pressed == Button::CONFIRM) {
                difficulty = (Difficulty)menu_selected_index;
                save_last_difficulty(difficulty);
                c4_init(&board);
                selected_col = 3;
                state = GameBrainState::HUMAN_TURN;
                send_face(FACE_IDLE);
                send_selector(selected_col);
            }
            break;

        case GameBrainState::HUMAN_TURN:
            if (pressed == Button::LEFT && selected_col > 0) {
                selected_col--;
                send_selector(selected_col);
            } else if (pressed == Button::RIGHT && selected_col < C4_WIDTH - 1) {
                selected_col++;
                send_selector(selected_col);
            } else if (pressed == Button::CONFIRM) {
                if (c4_is_column_legal(&board, selected_col)) {
                    Serial.print("Human chose column ");
                    Serial.println(selected_col);
                    send_drop(selected_col, PLAYER_RED);
                }
            }
            break;

        case GameBrainState::AI_TURN: {
            Serial.println("AI searching...");
            SearchResult result = get_ai_move(&board, &DIFFICULTY_PRESETS[difficulty]);
            Serial.print("AI chose column ");
            Serial.print(result.move);
            Serial.print("  value=");
            Serial.println(result.value);
            classify_and_send_reaction(result);
            send_drop(result.move, PLAYER_BLACK);
            break;
        }

        case GameBrainState::AWAITING_DROP:
            break;

        case GameBrainState::GAME_OVER:
            if (pressed == Button::CONFIRM) {
                c4_init(&board);
                send_home();
            }
            break;

        case GameBrainState::FAULT:
            break;
    }
}