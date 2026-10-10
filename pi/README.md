# Connect Four Game Brain (Raspberry Pi Zero) + trimmed Motion Controller

## Layout
- `src/`  Pi code. `protocol.h` and `board.h` are the validated ESP32 files, unchanged.
  - `ai.cpp/.h` the minimax (identical search to the ESP32 `minimax.h`, plus a cancel flag)
  - `game.*` Board wrapper, difficulty presets, opening book, Noob blunder
  - `reaction.h` face reaction from the search value
  - `link.*` UART (termios) and `SimLink` (fake Motion Controller)
  - `brain.*` the state machine (no rendering)
  - `settings.*` settings file (replaces NVS), `main.cpp` console runner, `bench.cpp`
- `motion_controller/` PlatformIO project, motion only. `host_test/` runs it on a PC with stubs.

## Laptop
    make && make test
    ./brain --sim          # a/d = left/right, Enter = confirm, q = quit

## Pi Zero (build ON the Pi)
    sudo apt install build-essential
    make
    ./bench                # run this first; compare with the ESP32 (hard position depth 10 = ~2.4 s there)
    sudo raspi-config      # Interface Options > Serial: login shell NO, serial hardware YES, then reboot
    ./brain --serial /dev/serial0

## Wiring
Pi GPIO14 (TX) -> ESP32 GPIO14 (RX); Pi GPIO15 (RX) <- ESP32 GPIO5 (TX); shared GND. 3.3 V logic both sides.
