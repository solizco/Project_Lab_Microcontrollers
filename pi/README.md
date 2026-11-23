# R.O.G.E.R. Game Brain (Raspberry Pi)

C++17 source for the program that runs R.O.G.E.R.: the game, the minimax AI, the menus and face animations on the HDMI screen, and, on the Pi 4, the gantry itself.

One program supports three setups, chosen at start-up:

| Flag | Setup | Gantry |
|---|---|---|
| `--gpio` | **Pi 4 only (final)** | Driven from the Pi's GPIO. Pins are read from `pins.conf`. |
| `--serial <port>` | Pi Zero / Pi 5 + ESP32 | ESP32 Motion Controller over UART (`/dev/serial0`, or `/dev/ttyAMA0` on a Pi 5) |
| `--sim` | anywhere | Simulated gantry; play from the keyboard |

## Source layout

| File | What it does |
|---|---|
| `src/ai.*` | Minimax with alpha-beta pruning on bitboards, plus a cancel flag (same search as the ESP32's `minimax.h`) |
| `src/game.*` | Board wrapper, difficulty presets (Noob / Pro / Hacker), opening book, Noob blunders |
| `src/brain.*` | The game state machine (no drawing) |
| `src/app.*`, `src/gfx.*`, `src/faces.*`, `src/font_data.h` | Menus and screens drawn straight to the Linux framebuffer; `.face` animation player |
| `src/link.*` | The link to the gantry: UART (termios) or `SimLink` |
| `src/motion.*` | **The gantry on the Pi:** the ESP32 Motion Controller firmware moved into the Pi program. It runs on its own thread behind `MotionHal` and still speaks the same command frames, so the rest of the program didn't change. |
| `src/pi_hal.*` | `MotionHal` for a real Pi 4/5: GPIO through `/dev/gpiochipN` (no libraries, no root), servo through hardware PWM (`/sys/class/pwm`) with a software-PWM fallback |
| `src/pins.*` | Reads and checks `pins.conf` |
| `src/protocol.h`, `src/board.h` | Frame format and bitboard, shared with the ESP32 code |
| `src/settings.*`, `src/stats.*` | Settings file and win/loss statistics |
| `src/main.cpp`, `src/bench.cpp` | Program entry point; AI speed benchmark |
| `tests/` | Tests for the AI, link, game state machine, gantry (against a simulated carriage), PWM and UI |
| `motion_controller/` | ESP32 motion-only firmware (breadboard wiring) with a `host_test/` harness that runs it on a PC |
| `tools/` | `build_pi.sh` (cross-compile), `install_autostart.sh` (start at boot), `make_font.py` |

## Build

**On a laptop, to test with the simulated gantry:**

    make && make test
    ./brain --sim          # arrows or a/d move, Enter select, Esc back, Q quit

`test_ui` reads face files from `/tmp/roger/faces` by default. Pass a folder to use a different one: `build/test_ui ../roger/faces`.

**For the Pi, cross-compiled on a laptop** (needs `pip install ziglang`; nothing has to be installed on the Pi):

    sh tools/build_pi.sh pi64    # Pi 4 / Pi 5, 64-bit OS  -> out/pi64/c4/
    sh tools/build_pi.sh         # Pi Zero, 32-bit OS      -> out/c4/

Copy the `c4` folder onto the SD card. Setup, wiring and troubleshooting are in [`../roger/c4/README.txt`](../roger/c4/README.txt), and the Pi 4 pin map is in [`../roger/pins.conf`](../roger/pins.conf).
