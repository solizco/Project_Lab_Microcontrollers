#pragma once
#include <stdint.h>

// ============================================================
// Game Brain <-> Motion Controller UART Protocol
// ============================================================
// This header is shared verbatim by BOTH ESP32-WROOM-32E boards --
// copy it into both PlatformIO project's `include/` folders, or set
// them up to share a common lib/ directory. Keeping them byte-for-byte
// identical is what guarantees both sides agree on frame layout.
//
// DESIGN GOALS:
//   - Game Brain has FULL authority over game state. Motion Controller
//     is a dumb executor: it knows how to move a gantry and drop a
//     piece, and nothing about Connect Four rules, whose turn it is,
//     or what a win looks like.
//   - Every command Game Brain sends gets an explicit reply (DONE or
//     ERROR) -- Game Brain should never assume a drop succeeded just
//     because it sent the command. It must wait for confirmation
//     before updating its board state or display.
//   - A simple checksum catches UART noise/corruption. Games are slow
//     (seconds between moves), so we can afford a little protocol
//     overhead in exchange for reliability -- this is not a
//     high-throughput link.

// ------------------------------------------------------------
// Frame format (byte layout on the wire)
// ------------------------------------------------------------
//   [ START ][ CMD ][ LEN ][ PAYLOAD (0-4 bytes) ][ CHECKSUM ][ END ]
//
//   START    : always 0xAA -- lets the receiver resynchronize if it
//              starts listening mid-frame or after a dropped byte
//   CMD      : one of the C4Command values below
//   LEN      : number of payload bytes that follow (0-4)
//   PAYLOAD  : command-specific data, see each command's comment
//   CHECKSUM : XOR of CMD, LEN, and every payload byte -- lets the
//              receiver detect a corrupted frame and discard it
//              rather than acting on bad data
//   END      : always 0x55 -- a second framing marker; if this isn't
//              where we expect it, something's wrong with the frame
//              and it should be discarded

// ------------------------------------------------------------
// FaceEvent (payload of CMD_FACE)
// ------------------------------------------------------------
// Named from the robot personality's own point of view -- FACE_WIN/
// FACE_LOSS are about the AI's own game outcome, not the human's.
enum FaceEvent : uint8_t {
    FACE_IDLE      = 0x00,  // default/neutral, also used to explicitly
                              // clear a reaction back to idle
    FACE_THINKING  = 0x01,  // sent right before the search starts --
                              // Motion Controller owns how long this
                              // plays; Game Brain doesn't track or care
    FACE_GOOD_MOVE = 0x02,  // the AI's search, right after the human's
                              // move, does NOT see a clear advantage --
                              // reacting to the human having played well
    FACE_BAD_MOVE  = 0x03,  // the AI's search, right after the human's
                              // move, finds a forced win or a strong
                              // heuristic advantage
    FACE_WIN       = 0x04,  // the AI won the game
    FACE_LOSS      = 0x05,  // the AI lost the game
};

#define C4_FRAME_START   0xAA
#define C4_FRAME_END     0x55
#define C4_MAX_PAYLOAD   4

// ------------------------------------------------------------
// SelectedMode (payload of CMD_MODE_SELECTED)
// ------------------------------------------------------------
// Matches Motion Controller's own local menu enum ordering exactly --
// PASS_PLAY means two humans, no AI turn ever runs. NOOB/PRO/HACKER
// map to Difficulty in difficulty.h (subtract 1 to convert).
enum SelectedMode : uint8_t {
    MODE_PASS_PLAY = 0,
    MODE_NOOB      = 1,
    MODE_PRO       = 2,
    MODE_HACKER    = 3,
};

// ------------------------------------------------------------
// Commands
// ------------------------------------------------------------
enum C4Command : uint8_t {
    // ---- Game Brain -> Motion Controller ----
    CMD_HOME  = 0x01,  // Return the gantry to its home/reference position.
                        // Sent once at power-on handshake time and at the
                        // start of every new game, so both boards start
                        // from a known physical state. No payload.

    CMD_DROP  = 0x02,  // Drop one piece.
                        // payload[0] = column (0-6)
                        // payload[1] = color  (0 = RED, 1 = BLACK)
                        // Motion Controller should NOT begin this until
                        // any prior DROP has been fully completed
                        // (DONE or ERROR sent) -- Game Brain will not
                        // send a new DROP before that happens, but
                        // Motion Controller should treat an out-of-turn
                        // DROP as a protocol error regardless, not
                        // silently queue it.

    CMD_FACE  = 0x03,  // Show a personality reaction on the display.
                        // payload[0] = one of the FaceEvent values below.
                        // FIRE-AND-FORGET: no reply expected, and Game
                        // Brain does not wait for or check one. The whole
                        // point of housing the display on Motion
                        // Controller is so Game Brain's CPU stays fully
                        // dedicated to search -- adding a handshake here
                        // would defeat that. If a CMD_FACE frame gets
                        // lost to noise, the worst outcome is a missed
                        // animation cue, never a stuck game.

    CMD_MENU  = 0x04,  // Show the difficulty menu screen.
                        // payload[0] = currently highlighted index (0-2,
                        // matching Difficulty enum in difficulty.h).
                        // Sent once when entering MENU state, and again
                        // every time LEFT/RIGHT changes the selection.
                        // Fire-and-forget, same as CMD_FACE.

    CMD_SELECTOR = 0x05, // Show/update the column-selector indicator
                        // during the human's turn (Roger's screen mode,
                        // not the menu). payload[0] = column (0-6). Sent
                        // once entering HUMAN_TURN, and again every time
                        // LEFT/RIGHT changes the selection. Fire-and-
                        // forget, same as CMD_FACE.

    CMD_FAULT = 0x06,  // Show a fault/error screen. payload[0] = an
                        // error code -- either one of the C4Error values
                        // below (when relaying a Motion-Controller-
                        // reported fault) or 0xFF (a fault Game Brain
                        // detected on its own, e.g. a CMD_DONE column
                        // mismatch). Fire-and-forget, same as CMD_FACE --
                        // this is purely a display cue, not a substitute
                        // for CMD_ERROR's own handshake-free reporting.

    // ---- Motion Controller -> Game Brain ----
    // Buttons are physically wired to Motion Controller (co-located
    // with the shared screen), so Game Brain has no direct button
    // access at all -- every human input arrives as one of these two.

    CMD_MODE_SELECTED = 0x07, // Human confirmed a mode from the LOCAL
                        // menu that Motion Controller draws and
                        // navigates entirely on its own -- Game Brain
                        // has no involvement until this arrives.
                        // payload[0] = one of the SelectedMode values
                        // below. Only meaningful while Game Brain is
                        // in MENU state; starts the game on receipt.

    CMD_BUTTON = 0x08,  // Forward a human button press during
                        // HUMAN_TURN. payload[0] = 0:LEFT, 1:RIGHT,
                        // 2:CONFIRM. Ignored by Game Brain if it
                        // arrives in any other state (e.g. during
                        // AI_TURN, there's nothing for a button press
                        // to do).

    CMD_READY = 0x10,  // Homing complete, ready to accept commands.
                        // Sent once after boot-up homing finishes, and
                        // again after every CMD_HOME. Game Brain should
                        // not send CMD_DROP until it has seen this.
                        // No payload.

    CMD_DONE  = 0x11,  // The most recent DROP completed successfully --
                        // piece has physically left the mechanism and
                        // landed in the board.
                        // payload[0] = column that was dropped (echoed
                        //              back, so Game Brain can sanity-
                        //              check it matches what it sent)

    CMD_ERROR = 0x12,  // Something went wrong executing the last command.
                        // payload[0] = one of the C4Error codes below.
                        // Game Brain should treat this as a fault
                        // condition -- halt the game and show an error
                        // state on the display rather than silently
                        // continuing with a board state that may not
                        // match physical reality.
};

// ------------------------------------------------------------
// Error codes (payload of CMD_ERROR)
// ------------------------------------------------------------
enum C4Error : uint8_t {
    ERR_STEPPER_STALL  = 0x01,  // Gantry didn't reach target position
                                  // (e.g. missed steps, physical obstruction)
    ERR_SERVO_TIMEOUT  = 0x02,  // Servo didn't complete its release motion
                                  // in the expected time window
    ERR_INVALID_CMD    = 0x03,  // Received a command byte that isn't a
                                  // recognized C4Command -- almost always
                                  // means a framing/checksum bug, since a
                                  // corrupted frame should be caught by
                                  // the checksum before this point
    ERR_NOT_HOMED      = 0x04,  // Received CMD_DROP before homing ever
                                  // completed -- Game Brain sent a drop
                                  // too early in the boot sequence
};

// Game-Brain-side-only sentinel codes, never sent BY Motion Controller
// (used with send_fault(), which just tells the display what to show --
// not a real C4Error, but reuses the same payload slot):
//   0xFF -- CMD_DONE arrived with a mismatched column (boards desynced)
//   0xFE -- no CMD_READY after repeated CMD_HOME retries (likely UART
//           noise/wiring, or Motion Controller not actually running)
//   0xFD -- no CMD_DONE/CMD_ERROR after a CMD_DROP (unknown physical
//           state -- deliberately NOT retried, since resending a DROP
//           risks a second real piece dropping if the original actually
//           succeeded and only the reply was lost)

// ------------------------------------------------------------
// build_frame: Construct a complete frame ready to write to Serial
// ------------------------------------------------------------
// Returns the total frame length in bytes (always payload_len + 5),
// or 0 if payload_len exceeds C4_MAX_PAYLOAD (caller error -- every
// real command in this protocol uses at most 2 payload bytes, so this
// should never trigger unless something is badly wrong upstream).
//
// `out` must point to a buffer of at least C4_MAX_PAYLOAD + 5 bytes.
inline uint8_t build_frame(uint8_t cmd, const uint8_t* payload,
                           uint8_t payload_len, uint8_t* out) {
    if (payload_len > C4_MAX_PAYLOAD) return 0;

    uint8_t idx = 0;
    out[idx++] = C4_FRAME_START;
    out[idx++] = cmd;
    out[idx++] = payload_len;

    uint8_t checksum = cmd ^ payload_len;
    for (uint8_t i = 0; i < payload_len; i++) {
        out[idx++] = payload[i];
        checksum ^= payload[i];
    }

    out[idx++] = checksum;
    out[idx++] = C4_FRAME_END;
    return idx;  // total bytes written
}

// ------------------------------------------------------------
// FrameParser: Byte-at-a-time receiver state machine
// ------------------------------------------------------------
// UART data arrives one byte at a time (via Serial.available() polling
// or an RX interrupt) -- this can't just "read a whole frame" in one
// call, since bytes trickle in over time. Feed each incoming byte to
// feed_byte(); it returns true exactly once, on the byte that
// completes a valid frame. At that point, cmd/payload/payload_len
// hold the decoded frame; read them out before calling feed_byte()
// again, since the next call starts decoding the NEXT frame and will
// overwrite them.
//
// Any frame that fails the checksum, or where START/END don't line up
// where expected, is silently discarded and the parser resets itself
// to look for the next 0xAA -- this is what lets the link recover from
// a dropped or corrupted byte without needing an explicit resync
// command from either side.
class FrameParser {
public:
    uint8_t cmd = 0;
    uint8_t payload[C4_MAX_PAYLOAD] = {0};
    uint8_t payload_len = 0;

    // Feed one incoming byte. Returns true if this byte completed a
    // valid frame (cmd/payload/payload_len are ready to read).
    bool feed_byte(uint8_t b) {
        switch (state) {
            case WAIT_START:
                if (b == C4_FRAME_START) state = WAIT_CMD;
                // else: not a start byte -- ignore and keep waiting.
                // This is the resync behavior: a stray byte before a
                // real frame just gets dropped here, no harm done.
                break;

            case WAIT_CMD:
                cmd = b;
                state = WAIT_LEN;
                break;

            case WAIT_LEN:
                if (b > C4_MAX_PAYLOAD) {
                    // Malformed length -- abandon this frame attempt
                    // and go back to looking for a fresh start byte,
                    // rather than trying to read a payload size that
                    // can't be real.
                    state = WAIT_START;
                } else {
                    payload_len = b;
                    payload_idx = 0;
                    state = (payload_len == 0) ? WAIT_CHECKSUM : WAIT_PAYLOAD;
                }
                break;

            case WAIT_PAYLOAD:
                payload[payload_idx++] = b;
                if (payload_idx >= payload_len) state = WAIT_CHECKSUM;
                break;

            case WAIT_CHECKSUM: {
                uint8_t expected = cmd ^ payload_len;
                for (uint8_t i = 0; i < payload_len; i++) expected ^= payload[i];
                if (b == expected) {
                    state = WAIT_END;
                } else {
                    // Checksum mismatch -- corrupted frame, discard
                    // and resync rather than trusting bad data.
                    state = WAIT_START;
                }
                break;
            }

            case WAIT_END:
                state = WAIT_START;  // always reset for the next frame,
                                       // whether this one was valid or not
                if (b == C4_FRAME_END) {
                    return true;  // complete, valid frame -- caller
                                   // should read cmd/payload/payload_len now
                }
                break;
        }
        return false;
    }

private:
    enum ParseState {
        WAIT_START, WAIT_CMD, WAIT_LEN, WAIT_PAYLOAD, WAIT_CHECKSUM, WAIT_END
    };
    ParseState state = WAIT_START;
    uint8_t payload_idx = 0;
};