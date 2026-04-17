#pragma once
#include <stdint.h>

// Per-frame WRAM dumper.
// Callback fired with mine!=NULL (recomp WRAM), theirs==NULL each frame.
//
// Output layout:
//   <dir>/recomp/frame_NNNNNN_wram.bin  — 128 KB recomp WRAM
//   <dir>/oracle/frame_NNNNNN_wram.bin  — 128 KB oracle WRAM
//   <dir>/recomp/frame_NNNNNN.json      — key state + crc32
//   <dir>/oracle/frame_NNNNNN.json      — key state + crc32

typedef void (*FrameDumpCallback)(uint32_t frame,
    const uint8_t *wram_mine, const uint8_t *wram_theirs);

extern FrameDumpCallback g_framedump_callback;

void FrameDump_Init(const char *dir);
