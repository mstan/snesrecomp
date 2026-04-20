#pragma once
#include "types.h"

// RecompHwState: Recomp-owned hardware state.
// Currently holds just the fields the recomp hot path actually reads.
// The CPU-register mirror (a/x/y/sp/dp/pc/k/db/flags/e/mf/xf) was write-only
// scaffolding and has been removed.
typedef struct RecompHwState {
  // IRQ/timing (driven from $4200/$4207 writes in recomp_hw)
  bool vIrqEnabled;
  uint16 vTimer;

  // Joypad input (set by RtlRunFrame from the game's input callback)
  uint16 input1, input2;

  // WRAM access port state (for registers 0x2180-0x2183)
  uint32 wramAddr;
} RecompHwState;

extern RecompHwState g_recomp;

// Sync g_recomp from emulator state (g_cpu, g_snes).
// Call at frame boundaries or before code that reads g_recomp.
void recomp_sync_from_emu(void);
