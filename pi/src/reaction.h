#pragma once
// Face reaction chosen from the search value on the AI's turn.
// Same thresholds as the ESP32 classify_and_send_reaction; it now just
// returns the face instead of sending CMD_FACE.
#include <random>
#include "ai.h"
#include "protocol.h"

class ReactionClassifier {
public:
    // value is from the AI's point of view. Returns FACE_IDLE for "no reaction".
    FaceEvent classify(int32_t value, std::mt19937& rng) {
        bool proven_win = value >= (int32_t)(C4_TERMINAL_BIG - 42);
        FaceEvent candidate = FACE_IDLE;
        uint8_t chance = 0;
        if (proven_win)        { candidate = FACE_BAD_MOVE;  chance = 70; }
        else if (value > 40)   { candidate = FACE_BAD_MOVE;  chance = 45; }
        else if (value < -10)  { candidate = FACE_GOOD_MOVE; chance = 45; }

        if (candidate == FACE_IDLE) { last_ = FACE_IDLE; return FACE_IDLE; }
        if (candidate == last_) chance /= 2;             // same reaction twice: halve the odds
        if ((int)(rng() % 100) < chance) { last_ = candidate; return candidate; }
        return FACE_IDLE;
    }
    void reset() { last_ = FACE_IDLE; }
    // R.O.G.E.R's verdict on the human's move, from a quick search of the
    // position after it (value from R.O.G.E.R's side). Every move gets one.
    static FaceEvent judge(int32_t value) { return value > 15 ? FACE_BAD_MOVE : FACE_GOOD_MOVE; }
private:
    FaceEvent last_ = FACE_IDLE;
};
