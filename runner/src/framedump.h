#pragma once
#include <stdint.h>

// Per-frame WRAM dumper (game-agnostic).
// Output layout:
//   <dir>/frame_NNNNNN_wram.bin  — 128 KB recomp WRAM
//   <dir>/frame_NNNNNN.json      — frame + wram_size + crc32
// Per-game decoders should read the .bin for game-specific fields.

typedef void (*FrameDumpCallback)(uint32_t frame, const uint8_t *wram);

extern FrameDumpCallback g_framedump_callback;

void FrameDump_Init(const char *dir);
