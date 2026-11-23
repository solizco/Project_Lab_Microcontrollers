#!/bin/sh
# Cross-compile static binaries for the Pi from a laptop (no compiler needed on the Pi).
# Needs: pip install ziglang
#   sh tools/build_pi.sh          Pi Zero (ARMv6, 32-bit OS)   -> out/c4/
#   sh tools/build_pi.sh pi64     Pi 4 / Pi 5 (64-bit OS)      -> out/pi64/c4/
set -e
cd "$(dirname "$0")/.."
Z="python3 -m ziglang"
if [ "${1:-}" = "pi64" ] || [ "${1:-}" = "pi4" ] || [ "${1:-}" = "pi5" ]; then
    # Cortex-A72 (Pi 4) instructions only, so the same file also runs on the Pi 5 (A76).
    T="-target aarch64-linux-musl -mcpu=cortex_a72"
    OUT=out/pi64/c4; OBJ=out/obj_pi64; TAG="_pi64"
else
    T="-target arm-linux-musleabihf -mcpu=arm1176jzf_s"
    OUT=out/c4; OBJ=out/obj; TAG=""
fi
mkdir -p $OUT $OBJ
DEF="deflate_decompress zlib_decompress adler32 utils arm/cpu_features"
DOBJ=""
for f in $DEF; do o=$OBJ/$(echo $f | tr / _).o; $Z cc $T -O2 -Ithird_party/libdeflate -c third_party/libdeflate/lib/$f.c -o $o; DOBJ="$DOBJ $o"; done
STAMP="$(date +%Y-%m-%d_%H:%M)$TAG"
CXX="$Z c++ $T -std=c++17 -O2 -static -s -Ithird_party/libdeflate -DC4_BUILD=\"$STAMP\""
echo "build stamp: $STAMP"
LIB="src/ai.cpp src/game.cpp src/settings.cpp src/link.cpp src/brain.cpp src/pins.cpp src/motion.cpp"
UI="src/gfx.cpp src/faces.cpp src/stats.cpp src/app.cpp src/pi_hal.cpp"
$CXX $LIB $UI src/main.cpp $DOBJ -o $OUT/brain
$CXX src/ai.cpp src/bench.cpp -o $OUT/bench
$CXX tests/test_ai.cpp src/ai.cpp -o $OUT/test_ai
$CXX tests/test_brain.cpp $LIB -o $OUT/test_brain
$CXX tests/test_motion.cpp $LIB -o $OUT/test_motion
$CXX tests/test_pwm.cpp src/pi_hal.cpp $LIB -o $OUT/test_pwm
$CXX tests/test_ui.cpp $LIB $UI $DOBJ -o $OUT/test_ui
ls -la $OUT
