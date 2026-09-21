#include <Arduino.h>
#include "protocol.h"
#include "board.h"
#include "minimax.h"
#include "difficulty.h"

// ============================================================
// Game Brain: Main State Machine
// ============================================================
// Buttons and the mode/difficulty menu now live entirely on Motion
// Controller, co-located with the shared screen -- Game Brain has NO
// direct button access at all. Every human input arrives as a
// fire-and-forget UART command:
//   CMD_MODE_SELECTED -- sent once, when the human confirms a mode on
//                         Motion Controller's own local menu (which
//                         Game Brain has zero involvement in drawing
//                         or navigating)
//   CMD_BUTTON         -- forwards a LEFT/RIGHT/CONFIRM press during
//                         HUMAN_TURN or GAME_OVER
// Game Brain still sends its own fire-and-forget display cues back
// (CMD_SELECTOR, CMD_FACE, CMD_FAULT) -- neither side ever waits on or
// checks for a reply to any of these, so a missed frame costs a
// missed visual cue, never a stuck game.
//
// PASS_PLAY mode: two humans, no AI turn ever runs. Both colors are
// human-controlled via CMD_BUTTON; the search is never invoked.
//
// ADJUST these UART pins/baud to match the actual wiring to Motion
// Controller once Guyton's PCB is finalized -- placeholders for now.
#define MOTION_UART_BAUD 115200
#define MOTION_UART_RX   16
#define MOTION_UART_TX   17

enum class GameBrainState {
    HOMING,         // sent CMD_HOME, waiting for Motion Controller's CMD_READY
    MENU,           // waiting for CMD_MODE_SELECTED (Motion Controller
                     // owns the actual menu UI and navigation)
    HUMAN_TURN,     // waiting for a forwarded CMD_BUTTON to pick a column
    AI_TURN,        // computing the bot's move (never entered in PASS_PLAY)
    AWAITING_DROP,  // CMD_DROP sent, waiting for CMD_DONE or CMD_ERROR
    GAME_OVER,      // showing result, waiting for a forwarded CONFIRM
                     // to play again
    FAULT,          // a fault occurred -- halted, requires a physical
                     // power cycle to recover
};

GameBrainState state = GameBrainState::HOMING;
FrameParser parser;

// ------------------------------------------------------------
// state_name: for readable logging only
// ------------------------------------------------------------
const char* state_name(GameBrainState s) {
    switch (s) {
        case GameBrainState::HOMING:        return "HOMING";
        case GameBrainState::MENU:          return "MENU";
        case GameBrainState::HUMAN_TURN:    return "HUMAN_TURN";
        case GameBrainState::AI_TURN:       return "AI_TURN";
        case GameBrainState::AWAITING_DROP: return "AWAITING_DROP";
        case GameBrainState::GAME_OVER:     return "GAME_OVER";
        case GameBrainState::FAULT:         return "FAULT";
    }
    return "?";
}

C4State board;
Difficulty difficulty;
bool pass_play = false;               // true: two humans, no AI turn ever
uint8_t selected_col = 0;             // column selector during HUMAN_TURN,
                                        // starts leftmost, matching the
                                        // physical home position
uint8_t pending_col;                  // column just sent to Motion Controller,
                                        // awaiting confirmation
C4Player pending_mover;               // who made the pending move -- captured
                                        // BEFORE applying it, since to_move
                                        // flips inside c4_apply_move()

// ------------------------------------------------------------
// Comms resilience: timeouts for the two states that gate on a reply
// ------------------------------------------------------------
unsigned long state_entered_at = 0;
uint8_t home_retry_count = 0;
#define HOME_RETRY_TIMEOUT_MS 20000
#define HOME_MAX_RETRIES       3
#define DROP_TIMEOUT_MS        5000

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
    Serial.print("SENT: CMD_FACE evt=");
    Serial.println(evt);
}

void send_selector(uint8_t col) {
    uint8_t payload[1] = { col };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_SELECTOR, payload, 1, frame);
    Serial2.write(frame, len);
    Serial.print("SENT: CMD_SELECTOR col=");
    Serial.println(col);
}

void send_fault(uint8_t error_code) {
    uint8_t payload[1] = { error_code };
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_FAULT, payload, 1, frame);
    Serial2.write(frame, len);
    Serial.print("SENT: CMD_FAULT code=0x");
    Serial.println(error_code, HEX);
}

// ------------------------------------------------------------
// classify_and_send_reaction -- never called in PASS_PLAY (no AI to react)
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
    state_entered_at = millis();
    Serial.print("SENT: CMD_DROP col=");
    Serial.print(col);
    Serial.print(" mover=");
    Serial.print(mover == PLAYER_RED ? "RED" : "BLACK");
    Serial.println("  -> AWAITING_DROP");
}

void send_home() {
    uint8_t frame[C4_MAX_PAYLOAD + 5];
    uint8_t len = build_frame(CMD_HOME, nullptr, 0, frame);
    Serial2.write(frame, len);
    state = GameBrainState::HOMING;
    state_entered_at = millis();
    Serial.println("SENT: CMD_HOME  -> HOMING");
}

// ------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial2.begin(MOTION_UART_BAUD, SERIAL_8N1, MOTION_UART_RX, MOTION_UART_TX);

    c4_init(&board);

    home_retry_count = 0;
    send_home();
    Serial.println("Game Brain booted. Sent CMD_HOME, waiting for CMD_READY...");
}

void loop() {
    while (Serial2.available()) {
        if (parser.feed_byte(Serial2.read())) {
            Serial.print("RECV: cmd=0x");
            Serial.print(parser.cmd, HEX);
            Serial.print(" payload[0]=");
            Serial.print(parser.payload[0]);
            Serial.print("  (current state: ");
            Serial.print(state_name(state));
            Serial.println(")");

            if (parser.cmd == CMD_READY && state == GameBrainState::HOMING) {
                state = GameBrainState::MENU;
                Serial.println("Received CMD_READY -- entering MENU (waiting for CMD_MODE_SELECTED)");

            } else if (parser.cmd == CMD_MODE_SELECTED && state == GameBrainState::MENU) {
                uint8_t mode = parser.payload[0];
                pass_play = (mode == MODE_PASS_PLAY);
                c4_init(&board);
                selected_col = 0;

                if (pass_play) {
                    Serial.println("Mode selected: PASS & PLAY (two humans, no AI)");
                    state = GameBrainState::HUMAN_TURN;
                    send_face(FACE_IDLE);
                    send_selector(selected_col);
                } else {
                    difficulty = (Difficulty)(mode - 1); // MODE_NOOB=1 -> DIFF_NOOB=0, etc.
                    save_last_difficulty(difficulty);
                    Serial.print("Mode selected: difficulty index ");
                    Serial.println((uint8_t)difficulty);
                    state = GameBrainState::AI_TURN; // AI always moves first
                    send_face(FACE_THINKING);
                }

            } else if (parser.cmd == CMD_BUTTON && state == GameBrainState::HUMAN_TURN) {
                uint8_t btn = parser.payload[0]; // 0:LEFT, 1:RIGHT, 2:CONFIRM
                if (btn == 0 && selected_col > 0) {
                    selected_col--;
                    send_selector(selected_col);
                } else if (btn == 1 && selected_col < C4_WIDTH - 1) {
                    selected_col++;
                    send_selector(selected_col);
                } else if (btn == 2) {
                    if (c4_is_column_legal(&board, selected_col)) {
                        C4Player mover = board.to_move; // whichever color's
                                                           // turn it actually
                                                           // is (matters for
                                                           // PASS_PLAY)
                        Serial.print("Human (");
                        Serial.print(mover == PLAYER_RED ? "RED" : "BLACK");
                        Serial.print(") chose column ");
                        Serial.println(selected_col);
                        send_drop(selected_col, mover);
                    }
                }

            } else if (parser.cmd == CMD_BUTTON && state == GameBrainState::GAME_OVER) {
                if (parser.payload[0] == 2) { // CONFIRM -- play again
                    c4_init(&board);
                    home_retry_count = 0;
                    send_home();
                }

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
                        if (pass_play) {
                            send_face(FACE_IDLE);
                            Serial.println(pending_mover == PLAYER_RED
                                                ? "Player 1 (RED) WINS!"
                                                : "Player 2 (BLACK) WINS!");
                        } else {
                            // PLAYER_RED is the AI (moves first);
                            // PLAYER_BLACK is the human.
                            send_face(pending_mover == PLAYER_RED ? FACE_WIN : FACE_LOSS);
                            Serial.println(pending_mover == PLAYER_RED ? "AI WINS!" : "Human WINS!");
                        }
                        state = GameBrainState::GAME_OVER;
                    } else if (full) {
                        send_face(FACE_IDLE);
                        state = GameBrainState::GAME_OVER;
                        Serial.println("Board full -- DRAW");
                    } else {
                        if (pass_play) {
                            state = GameBrainState::HUMAN_TURN; // always --
                                                                   // both
                                                                   // sides
                                                                   // human
                        } else {
                            state = (pending_mover == PLAYER_RED)
                                        ? GameBrainState::HUMAN_TURN
                                        : GameBrainState::AI_TURN;
                        }
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

            } else {
                Serial.println("  (ignored -- unexpected for current state)");
            }
        }
    }

    switch (state) {
        case GameBrainState::HOMING:
            if (millis() - state_entered_at > HOME_RETRY_TIMEOUT_MS) {
                home_retry_count++;
                if (home_retry_count > HOME_MAX_RETRIES) {
                    Serial.println("FAULT: no CMD_READY after repeated CMD_HOME retries.");
                    Serial.println("Check UART wiring, shared ground, and whether Motion Controller is actually running.");
                    send_fault(0xFE);
                    state = GameBrainState::FAULT;
                } else {
                    Serial.print("No CMD_READY yet -- retrying CMD_HOME (attempt ");
                    Serial.print(home_retry_count);
                    Serial.println(")");
                    send_home();
                }
            }
            break;

        case GameBrainState::MENU:
            // Nothing to do here but wait for CMD_MODE_SELECTED, handled
            // above -- Motion Controller owns all menu navigation itself.
            break;

        case GameBrainState::HUMAN_TURN:
            // Nothing to do here but wait for a forwarded CMD_BUTTON,
            // handled above.
            break;

        case GameBrainState::AI_TURN: {
            Serial.println("AI searching...");
            SearchResult result = get_ai_move(&board, &DIFFICULTY_PRESETS[difficulty]);
            Serial.print("AI chose column ");
            Serial.print(result.move);
            Serial.print("  value=");
            Serial.println(result.value);
            classify_and_send_reaction(result);
            send_drop(result.move, board.to_move);
            break;
        }

        case GameBrainState::AWAITING_DROP:
            if (millis() - state_entered_at > DROP_TIMEOUT_MS) {
                Serial.println("FAULT: no CMD_DONE/CMD_ERROR after CMD_DROP -- unknown physical state.");
                Serial.println("Check whether the piece actually dropped before resuming.");
                send_fault(0xFD);
                state = GameBrainState::FAULT;
            }
            break;

        case GameBrainState::GAME_OVER:
            // Nothing to do here but wait for a forwarded CONFIRM,
            // handled above.
            break;

        case GameBrainState::FAULT:
            break;
    }
}