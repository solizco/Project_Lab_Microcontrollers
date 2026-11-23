# R.O.G.E.R. — Connect Four Robot

**ECE 3332 Microcontroller Project Lab · Texas Tech University · Fall 2026**

R.O.G.E.R. is a Connect Four console that plays you for real. You pick a column with three buttons. When it's R.O.G.E.R.'s turn, a minimax AI chooses a move, and a stepper-driven gantry carries a real piece over the board and drops it in. An animated face on the screen reacts to every move you make.

The hardware changed four times during the semester. This README explains each design, why the team moved on from it, and which code in this repository belongs to which design.

---

## Features

- **Game modes:** 1v1 (pass and play) or vs R.O.G.E.R. at three difficulties:

  | Difficulty | Search depth | Notes |
  |---|---|---|
  | Noob | 2 ply | plays a random legal move 25% of the time |
  | Pro | 8 ply | full alpha-beta search, no randomness |
  | Hacker | 10 ply | iterative deepening and a transposition table; effectively unbeatable |

- **Physical play:** a NEMA 17 stepper on a gantry (salvaged from an Ender 3 V3) moves to the chosen column, and a servo pushes a red or black piece from its silo into the hopper. After every drop, the gantry re-homes on a limit switch so position error never builds up.
- **Face reactions:** R.O.G.E.R.'s face idles during your turn, reacts to good and bad moves, and plays a win or loss animation at the end.
- **Game flow:** a coin toss decides who goes first, and win/loss statistics are saved between games. A screen-only mode lets you play on the display with the gantry parked.
- **Recovery tools:** fault screens offer re-home and restart options, a link-debug screen shows the board-to-board traffic, and a hold-to-shut-down button powers the Pi off cleanly.

---

## How the design changed

| # | Design | When | Game, AI and screen | Motion | Display |
|---|---|---|---|---|---|
| 1 | Two ESP32s | Sep 2026 | ESP32 "Game Brain" | ESP32 "Motion Controller" | 2.0″ ST7789 SPI TFT |
| 2 | Pi Zero + ESP32 | Oct 2026 | Raspberry Pi Zero | ESP32 Motion Controller | 5″ 800×480 HDMI |
| 3 | Pi 5 + ESP32 | Nov 2026 (attempted) | Raspberry Pi 5 | ESP32 Motion Controller | 5″ 800×480 HDMI |
| 4 | **Pi 4 only (final)** | Nov 2026 | **Raspberry Pi 4** | **the same Pi 4, through its GPIO** | 5″ 800×480 HDMI |

### Design 1: two ESP32-WROOM-32E boards

The original proposal used a single ESP32-C3 with a 5″ RA8875 touchscreen. The team instead split the work across two ESP32-WROOM-32E boards linked over UART:

- **Game Brain** ran the Connect Four rules, the minimax bot and the 2.0″ ST7789 display.
- **Motion Controller** ran the gantry: STEP/DIR to a TMC2209 driver, the drop servo, the limit switch and the three buttons.

The boards talked through a small framed protocol (`protocol.h`). Every message is fire-and-forget, so a lost frame costs a missed screen update, never a stuck game. During this stage the menu and screen moved over to the Motion Controller. The team also tried playing R.O.G.E.R.'s face animations from the ESP32's flash (`random_esp32_testing/`).

**Why it changed:** the face animations needed a much bigger screen than the 2.0″ TFT. A Raspberry Pi Zero can drive a 5″ 800×480 HDMI display directly, so the Pi Zero took over the Game Brain role. The existing C++ game and AI code moved over with it.

### Design 2: Raspberry Pi Zero + one ESP32

The Pi Zero became the Game Brain:
- It runs the game state machine, the minimax search, and R.O.G.E.R.'s faces on the 5″ HDMI screen, drawing straight to the Linux framebuffer.
- It talks to the ESP32 over the Pi's UART (GPIO14/15).
- The faces are pre-converted on a laptop into a compressed `.face` format, so the single-core Pi Zero only has to unpack one frame at a time.
- The Pi Zero has no network connection, so programs are cross-compiled on a laptop and copied onto the SD card.

The ESP32 was cut down to motion only: it homes, moves, drops and forwards button presses. It knows nothing about Connect Four.

**Why it changed:** on the 5th revision of the custom PCB, both the Pi Zero and the ESP32 were destroyed.

### Design 3: Raspberry Pi 5 + ESP32 (attempted)

The first replacement plan kept the same architecture with a Raspberry Pi 5 in the Pi Zero's place. The game was rebuilt as a 64-bit binary. The installer switched to the Pi 5's serial port (`/dev/ttyAMA0`) and added autostart, clean shutdown and recovery features. The Motion Controller firmware gained a status LED, self-restart on failed homing, and servo calibration over the serial port.

**Why it changed:** the replacement Pi 5 didn't work. With the ESP32 also gone and nothing else on hand, the team moved to a Raspberry Pi 4.

### Design 4: Raspberry Pi 4 only (final)

The final R.O.G.E.R. runs everything on a single Raspberry Pi 4:
- the game, the AI and the face display
- the stepper gantry, the drop servo, the limit switch and the buttons, all wired to the Pi's own GPIO

There is no ESP32 and no UART link. The same game program runs in a new `--gpio` mode. The ESP32 firmware's motion code was moved into the Pi program (`pi/src/motion.*`), where it runs on its own thread and still uses the same command frames, so the game state machine, screen-only mode and the link-debug screen didn't change. The Pi 4 build adds these changes:

- **Pins live in a config file.** They are read from `roger/pins.conf` at start-up, so rewiring needs no rebuild. `brain --check-pins` shows what each pin reads right now.
- **The servo runs on hardware PWM** (GPIO12) and is calibrated from an on-screen menu, which replaced the ESP32's serial console.
- **Safeguards that used to live on the ESP32 now run on the Pi:**
  - two-touch homing and a filtered limit switch, so motor noise can't fake a home
  - debounced buttons, with left + right pressed together acting as select
  - a lost-step check on every re-home
  - motor power-off after five idle minutes
  - under-voltage warnings

A failed re-home after a drop no longer ends the game. Removing the second board also removed the whole class of link faults the earlier designs had to handle.

**Final pin assignments** (BCM numbering, from [`roger/pins.conf`](roger/pins.conf)):

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| Stepper STEP | 27 | Button left | 15 |
| Stepper DIR | 17 | Button right | 24 |
| Stepper EN (active low) | 22 | Button select | 23 |
| Limit switch | 14 | Shutdown button (hold 2 s) | 25 |
| Servo (hardware PWM) | 12 | | |

`roger/c4/README.txt` is the full setup and troubleshooting guide for this build.

---

## Which code belongs to which design

| Path | Design | What it is |
|---|---|---|
| `MicroControllersTestSuitev2_fixed.ipynb` | all | Python prototype of the bot: game class, minimax search, heuristic tuning, benchmarks and a human-vs-AI playtest. The C++ search in every design is based on it. |
| `game_brain (unused now)/` | 1 | PlatformIO project for the ESP32 Game Brain: bitboard board (`board.h`), minimax (`minimax.h`), difficulty presets, ST7789 display and the UART protocol. Retired when the Pi Zero took over. |
| `random_esp32_testing/` | 1 | ESP32 experiment playing face clips from LittleFS flash (packed MJPEG clips; per-file opens cost about 60 ms each). |
| `pi/src/`, `pi/tests/` | 2 → **4** | C++ Game Brain, written for the Pi Zero and carried through to the final Pi 4: AI (`ai.*`), rules and difficulty (`game.*`), state machine (`brain.*`), UART link plus a simulated gantry (`link.*`), framebuffer graphics (`gfx.*`), faces (`faces.*`), stats and settings. The Pi 4 files are `motion.*` (the gantry, moved from the ESP32), `pi_hal.*` (GPIO and hardware PWM) and `pins.*` (reads `pins.conf`). Tests cover the AI, link, state machine, gantry, PWM and UI. See [`pi/README.md`](pi/README.md). |
| `pi/Makefile`, `pi/tools/` | 2 → **4** | `make && make test` on a laptop. `tools/build_pi.sh pi64` cross-compiles the Pi 4/5 binaries (and with no argument, the Pi Zero ones) using Zig. `install_autostart.sh` starts the game at boot. |
| `pi/motion_controller/` | 2–3 | A copy of the motion-only ESP32 firmware kept alongside the Pi source: breadboard wiring, different servo defaults (90 / 60 / 75 / 120 / 105 / 250), and a `host_test/` harness that runs it on a PC. |
| `roger/gifs/`, `roger/faces/` | 2–4 | R.O.G.E.R.'s face animations: the source GIFs and the converted `.face` files the game plays. |
| `roger/gifface.py`, `roger/convert_gifs.py`, `roger/convert.bat` | 2–4 | Face tooling: preview and adjust each GIF's framing and speed, then convert the GIFs to `.face` files on a laptop. |
| `roger/connect4.py` | 2 | Early Python/pygame prototype of the menu and statistics screens, later rebuilt in C++. |
| `roger/c4/` | **4** (also 3) | **The final build:** 64-bit binaries for the Pi 4 (and Pi 5), built Nov 21. Start it with `--gpio` (Pi 4 runs everything, the final setup) or `--serial` (with the ESP32 Motion Controller, Design 3). Includes the autostart installer, the `test_motion` and `test_pwm` hardware tests, and `README.txt`, the full setup and troubleshooting guide. The Pi-5-only build from Design 3 is in the git history (commit `3b10e3d`). |
| `roger/pins.conf` | 4 | GPIO wiring for the Pi 4, plus gantry, servo, homing and button tuning. Read at start-up. |
| `motion_controller/` | 1 → 3 | The ESP32 Motion Controller firmware in its final state (build 3), for the custom PCB wiring (`src/main.cpp`) and the breadboard wiring (`variants/breadboard_pins/`). Covers homing with automatic retry, drops, button and shutdown forwarding, link debugging and servo calibration. Retired in Design 4. See [`motion_controller/README.md`](motion_controller/README.md). |

`pi/` builds cleanly with `make`. On a PC, the AI, link, state machine (163 checks), gantry (109 checks) and PWM tests all pass. Cross-compiling with `tools/build_pi.sh pi64` reproduces the `roger/c4/` binaries.

---

## Board-to-board protocol (Designs 1–3)

The link runs over UART at 115200 baud. Each message is one frame: `[0xAA][CMD][LEN][payload, 0–4 bytes][XOR checksum][END]`. The fixed start byte lets the receiver resynchronize after noise.

| Direction | Commands |
|---|---|
| Game Brain → ESP32 | `CMD_HOME`, `CMD_DROP` (column + color), `CMD_SELECTOR` (follow the player's column), `CMD_PING`, `CMD_RESET` |
| ESP32 → Game Brain | `CMD_READY` (homed), `CMD_DONE`, `CMD_ERROR` (with error code), `CMD_BUTTON`, `CMD_SHUTDOWN`, `CMD_DEBUG` |

`CMD_FACE`, `CMD_MENU`, `CMD_FAULT` and `CMD_MODE_SELECTED` date from Design 1, when the ESP32 drew the screen; later firmware ignores them.

The full definitions are in `motion_controller/src/protocol.h`.

---

## Hardware

- **Controller:** Raspberry Pi 4 (final). Earlier designs used two ESP32-WROOM-32E boards, then a Pi Zero + ESP32, then an attempted Pi 5 + ESP32.
- **Display:** 5″ 800×480 HDMI (touch not used). Design 1 used a 2.0″ ST7789 SPI TFT.
- **Motion:** NEMA 17 stepper (200 steps/rev) with a TMC2209 driver on a salvaged Ender 3 V3 gantry, an SG995 drop servo, a limit switch for homing, and separate red and black piece silos.
- **Input:** left, right and select buttons (left + right together also selects), plus a hold-2-seconds shutdown button.
- **Power:** 7.2 V 3600 mAh NiMH pack with two adjustable buck converters (one for the motor driver, one for the logic).
- **PCB:** custom board designed in KiCad (5 revisions).

---

## Team

- **Nicholas:** firmware, AI and system integration
- **Finn:** firmware support, display integration and R.O.G.E.R.'s face animations
- **Guyton:** power, custom PCB and enclosure
