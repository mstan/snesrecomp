#!/usr/bin/env bash
# Build and run the ROM-free C regression harnesses on a POSIX host.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/c-tests"
CC="${CC:-gcc}"
mkdir -p "$OUT"

LAUNCHER_LIBS=()
case "$(uname -s)" in
    MINGW*|MSYS*) LAUNCHER_LIBS=(-lcomdlg32) ;;
esac

echo "=== launcher ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -D_POSIX_C_SOURCE=200809L -I "$ROOT/runner/src" \
    "$ROOT/tests/launcher/launcher_test.c" \
    "$ROOT/runner/src/launcher.c" \
    "$ROOT/runner/src/launcher_cache.c" \
    "$ROOT/runner/src/launcher_picker.c" \
    "$ROOT/runner/src/rom_image_verify.c" \
    "$ROOT/runner/src/host_paths.c" \
    "$ROOT/runner/src/crc32.c" \
    "$ROOT/runner/src/sha256.c" \
    "${LAUNCHER_LIBS[@]}" \
    -o "$OUT/launcher_test"
"$OUT/launcher_test"

echo "=== PPU sprite limits ==="
"$CC" -std=c11 -Wall -Wextra -O1 \
    -DSNESRECOMP_REVERSE_DEBUG=0 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/ppu/ppu_sprite_limit_test.c" \
    "$ROOT/runner/src/snes/ppu.c" \
    "$ROOT/runner/src/snes/ppu_legacy.c" \
    -o "$OUT/ppu_sprite_limit_test"
"$OUT/ppu_sprite_limit_test"

echo "=== DMA / HDMA ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -DSNESRECOMP_REVERSE_DEBUG=0 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dma/hdma_test.c" \
    "$ROOT/runner/src/snes/dma.c" \
    -o "$OUT/hdma_test"
"$OUT/hdma_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -Wno-error=parentheses -Wno-error=unused-variable \
    -Wno-error=unused-const-variable \
    -DSNESRECOMP_REVERSE_DEBUG=0 \
    -ffunction-sections -fdata-sections \
    -I "$ROOT/tests/dma" -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dma/hdma_timing_test.c" \
    "$ROOT/runner/src/snes/dma.c" \
    "$ROOT/runner/src/snes/snes.c" \
    -Wl,--gc-sections -o "$OUT/hdma_timing_test"
"$OUT/hdma_timing_test"

echo "=== interpreter and bridge ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -D_POSIX_C_SOURCE=200809L -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/interp816/tier2_capture_test.c" \
    "$ROOT/runner/src/snes/tier2_capture.c" \
    -o "$OUT/tier2_capture_test"
(cd "$OUT" && ./tier2_capture_test)

"$CC" -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 \
    -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/interp816/interp816_test.c" \
    "$ROOT/runner/src/snes/interp816.c" \
    -o "$OUT/interp816_test"
"$OUT/interp816_test"

"$CC" -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 \
    -D_POSIX_C_SOURCE=200809L -DSNESRECOMP_TIER2_TEST=1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/interp816/bridge_test.c" \
    "$ROOT/runner/src/snes/interp816.c" \
    "$ROOT/runner/src/snes/tier2_capture.c" \
    "$ROOT/runner/src/snes/interp_bridge.c" \
    "$ROOT/runner/src/snes/cx4.c" \
    -lm -o "$OUT/bridge_test"
"$OUT/bridge_test"

echo "=== DSP-1 bus/core shell ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dsp1/dsp1_header_test.c" \
    "$ROOT/runner/src/snes/snes_other.c" \
    -o "$OUT/dsp1_header_test"
"$OUT/dsp1_header_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dsp1/dsp1_test.c" \
    "$ROOT/runner/src/snes/dsp1.c" \
    "$ROOT/runner/src/snes/dsp1_hle.c" \
    -lm -o "$OUT/dsp1_test"
"$OUT/dsp1_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dsp1/dsp1_hle_test.c" \
    "$ROOT/runner/src/snes/dsp1_hle.c" \
    -lm \
    -o "$OUT/dsp1_hle_test"
"$OUT/dsp1_hle_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dsp1/dsp1_hle_host_test.c" \
    "$ROOT/runner/src/snes/dsp1.c" \
    "$ROOT/runner/src/snes/dsp1_hle.c" \
    -lm \
    -o "$OUT/dsp1_hle_host_test"
(cd "$OUT" && env -u SNESRECOMP_DSP1_ROM ./dsp1_hle_host_test)

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/dsp1/dsp1_firmware_test.c" \
    "$ROOT/runner/src/snes/dsp1.c" \
    "$ROOT/runner/src/snes/dsp1_hle.c" \
    -lm \
    -o "$OUT/dsp1_firmware_test"
"$OUT/dsp1_firmware_test"

echo "=== SA-1 CPU, mapping and peripherals ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/sa1/sa1_header_test.c" \
    "$ROOT/runner/src/snes/snes_other.c" \
    -o "$OUT/sa1_header_test"
"$OUT/sa1_header_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/sa1/sa1_test.c" \
    "$ROOT/runner/src/snes/sa1.c" \
    "$ROOT/runner/src/snes/interp816.c" \
    -o "$OUT/sa1_test"
"$OUT/sa1_test"

echo "=== manual joypad serial protocol ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/joypad/manual_joypad_test.c" \
    "$ROOT/runner/src/snes/joypad.c" \
    -o "$OUT/manual_joypad_test"
"$OUT/manual_joypad_test"

echo "=== automatic joypad register byte order ==="
"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/joypad/auto_joypad_test.c" \
    "$ROOT/runner/src/snes/joypad.c" \
    -o "$OUT/auto_joypad_test"
"$OUT/auto_joypad_test"

echo "=== runtime dispatch ==="
"$CC" -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/runtime_dispatch/known_lle_entry_test.c" \
    "$ROOT/runner/src/cpu_state.c" \
    "$ROOT/runner/src/snes/cart.c" \
    "$ROOT/runner/src/snes/cx4.c" \
    "$ROOT/runner/src/snes/dsp1.c" \
    "$ROOT/runner/src/snes/dsp1_hle.c" \
    "$ROOT/runner/src/snes/sa1.c" \
    "$ROOT/runner/src/snes/interp816.c" \
    -Wl,--gc-sections -lm -o "$OUT/known_lle_entry_test"
"$OUT/known_lle_entry_test"

echo "=== APU guest-time pacing ==="
"$CC" -std=c11 -Wall -Wextra -Werror \
    -Wno-error=unknown-pragmas -Wno-error=comment \
    -ffunction-sections -fdata-sections \
    -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
    "$ROOT/tests/runtime_dispatch/apu_port_guest_time_test.c" \
    "$ROOT/runner/src/snes/apu.c" \
    "$ROOT/runner/src/snes/spc.c" \
    "$ROOT/runner/src/snes/dsp.c" \
    -Wl,--gc-sections -o "$OUT/apu_port_guest_time_test"
"$OUT/apu_port_guest_time_test"
