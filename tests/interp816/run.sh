#!/usr/bin/env bash
# Build + run the interp816 / interp_bridge validation harnesses.
# Run from anywhere (e.g. under WSL): tests/interp816/run.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build
CFLAGS="-std=c11 -Wall -Wextra -Wno-unused-parameter -O1"

echo "=== Phase 0: interp816 core ==="
gcc $CFLAGS -I runner/src/snes \
    tests/interp816/interp816_test.c runner/src/snes/interp816.c \
    -o build/interp816_test
./build/interp816_test

echo ""
echo "=== Phase 1: interp_bridge contract ==="
gcc $CFLAGS -I runner/src -I runner/src/snes \
    tests/interp816/bridge_test.c \
    runner/src/snes/interp816.c runner/src/snes/interp_bridge.c \
    -o build/bridge_test
exec ./build/bridge_test
