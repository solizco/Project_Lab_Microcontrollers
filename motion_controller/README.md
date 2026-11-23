# ESP32 Motion Controller (retired)

PlatformIO firmware for the ESP32-WROOM-32E that ran R.O.G.E.R.'s gantry in Designs 1–3. In the final design the Raspberry Pi 4 drives the gantry itself (see `pi/src/motion.*`). This firmware is kept for reference.

It homes the gantry on the limit switch, moves to a column, drops a piece with the servo, re-homes after every drop, and forwards button presses to the Game Brain over UART (`src/protocol.h`). It knows nothing about Connect Four rules or the screen.

There are two pin variants. Each `main.cpp` goes in the PlatformIO project's `src/` folder next to `protocol.h`.

| Variant | File | EN | Limit | Servo | UART RX / TX | Servo defaults |
|---|---|---|---|---|---|---|
| Custom PCB | `src/main.cpp` | 33 | 32 | 27 | 18 / 5 | 103 / 70 / 100 / 135 / 110 / 250 |
| Breadboard | `variants/breadboard_pins/main.cpp` | 27 | 33 | 32 | 14 / 5 | 103 / 70 / 100 / 135 / 110 / 250 |

Both variants use STEP 25, DIR 26 (TMC2209), buttons 21 / 23 / 22, shutdown button 19 and status LED 2. `host_test/` runs the firmware on a PC with stub headers.
