#pragma once
#include <Arduino.h>

// ============================================================
// input.h -- Debounced Button Reading
// ============================================================
// Three physical buttons: LEFT, RIGHT, CONFIRM. The same three drive
// both the difficulty menu (cycle options, confirm selection) and
// in-game column selection (move a highlighted column left/right,
// confirm to drop) -- one input scheme, two contexts, matching how
// the Python test suite reused one search function across difficulties
// rather than writing separate code paths.
//
// ADJUST THESE PIN NUMBERS to match the actual PCB routing once
// Guyton's board is finalized -- these are placeholders.
#define PIN_BTN_LEFT    25
#define PIN_BTN_RIGHT   26
#define PIN_BTN_CONFIRM 27

#define DEBOUNCE_MS 30  // ignore state changes faster than this --
                          // filters mechanical switch bounce, which
                          // otherwise reads as several rapid presses
                          // for one physical press

enum class Button { NONE, LEFT, RIGHT, CONFIRM };

class ButtonReader {
public:
    void begin() {
        pinMode(PIN_BTN_LEFT, INPUT_PULLUP);
        pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
        pinMode(PIN_BTN_CONFIRM, INPUT_PULLUP);
        // INPUT_PULLUP: buttons wired to pull the pin LOW when
        // pressed (button connects pin to GND); the internal pullup
        // keeps the pin HIGH when unpressed, so no external resistor
        // is needed on the PCB. Adjust to INPUT + external pulldown
        // if the actual button wiring is the other polarity.
    }

    // Call every loop() iteration. Returns the button that was just
    // PRESSED this call (edge-triggered, not held) -- Button::NONE
    // most of the time. This edge-triggering is what prevents one
    // physical press from being read as dozens of repeated presses
    // while the button is held down across many loop() iterations.
    Button poll() {
        unsigned long now = millis();
        Button pressed = Button::NONE;

        pressed = check_pin(PIN_BTN_LEFT, Button::LEFT, now, pressed);
        pressed = check_pin(PIN_BTN_RIGHT, Button::RIGHT, now, pressed);
        pressed = check_pin(PIN_BTN_CONFIRM, Button::CONFIRM, now, pressed);

        return pressed;
    }

private:
    // Per-button debounce state
    bool last_reading[3] = {HIGH, HIGH, HIGH}; // raw pin state last seen
    bool debounced_state[3] = {HIGH, HIGH, HIGH}; // stable state after debounce
    unsigned long last_change_time[3] = {0, 0, 0};

    int pin_index(int pin) {
        if (pin == PIN_BTN_LEFT) return 0;
        if (pin == PIN_BTN_RIGHT) return 1;
        return 2; // PIN_BTN_CONFIRM
    }

    Button check_pin(int pin, Button which, unsigned long now, Button current_result) {
        int idx = pin_index(pin);
        bool raw = digitalRead(pin);

        if (raw != last_reading[idx]) {
            last_change_time[idx] = now; // reading changed -- restart
                                           // the debounce timer
            last_reading[idx] = raw;
        }

        if ((now - last_change_time[idx]) > DEBOUNCE_MS) {
            // Reading has been stable for long enough to trust it
            if (raw != debounced_state[idx]) {
                bool was_pressed = (debounced_state[idx] == HIGH && raw == LOW);
                debounced_state[idx] = raw;
                if (was_pressed) {
                    return which; // rising-to-falling edge = a genuine
                                    // press, report it
                }
            }
        }
        return current_result; // no new press on this pin -- pass
                                 // through whatever was already found
                                 // (if any) from an earlier pin this call
    }
};
