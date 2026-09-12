#!/usr/bin/env bash
# Build + run the interp816 / interp_bridge validation harnesses.
# Run from anywhere (e.g. under WSL): tests/interp816/run.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build
# _POSIX_C_SOURCE because -std=c11 is strict ISO: it hides setenv() (phase 1's
# bridge_test) and gmtime_r() (tier2_capture.c), which this harness has been
# failing to compile on since they were introduced. Phase 0 never touched
# either, so only phase 1 ever surfaced it.
CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -O1"

echo "=== Phase 0: interp816 core ==="
gcc $CFLAGS -I runner/src/snes \
    tests/interp816/interp816_test.c runner/src/snes/interp816.c \
    -o build/interp816_test
./build/interp816_test

echo ""
echo "=== Phase 1: interp_bridge contract ==="
gcc $CFLAGS -DSNESRECOMP_TIER2_TEST=1 -I runner/src -I runner/src/snes \
    tests/interp816/bridge_test.c \
    runner/src/snes/interp816.c runner/src/snes/interp_bridge.c \
    runner/src/snes/tier2_capture.c \
    runner/src/snes/cx4.c -lm -o build/bridge_test
exec ./build/bridge_test
