#!/bin/sh
# Cross-compile static ARMv6 binaries for the Pi Zero from a laptop (no compiler needed on the Pi).
# Needs: pip install ziglang     Output: out/c4/
set -e
cd "$(dirname "$0")/.."
Z="python3 -m ziglang"
T="-target arm-linux-musleabihf -mcpu=arm1176jzf_s"
OUT=out/c4; OBJ=out/obj; mkdir -p $OUT $OBJ
DEF="deflate_decompress zlib_decompress adler32 utils arm/cpu_features"
DOBJ=""
for f in $DEF; do o=$OBJ/$(echo $f | tr / _).o; $Z cc $T -O2 -Ithird_party/libdeflate -c third_party/libdeflate/lib/$f.c -o $o; DOBJ="$DOBJ $o"; done
CXX="$Z c++ $T -std=c++17 -O2 -static -s -Ithird_party/libdeflate"
LIB="src/ai.cpp src/game.cpp src/settings.cpp src/link.cpp src/brain.cpp"
UI="src/gfx.cpp src/faces.cpp src/stats.cpp src/app.cpp"
$CXX $LIB $UI src/main.cpp $DOBJ -o $OUT/brain
$CXX src/ai.cpp src/bench.cpp -o $OUT/bench
$CXX tests/test_ai.cpp src/ai.cpp -o $OUT/test_ai
$CXX tests/test_brain.cpp $LIB -o $OUT/test_brain
$CXX tests/test_ui.cpp $LIB $UI $DOBJ -o $OUT/test_ui
ls -la $OUT
