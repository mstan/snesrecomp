#pragma once
#include <stdint.h>

// Per-frame WRAM dumper.
// Output layout:
//   <dir>/frame_NNNNNN_wram.bin  — 128 KB recomp WRAM
//   <dir>/frame_NNNNNN.json      — key state + crc32

typedef void (*FrameDumpCallback)(uint32_t frame, const uint8_t *wram);

extern FrameDumpCallback g_framedump_callback;

void FrameDump_Init(const char *dir);
