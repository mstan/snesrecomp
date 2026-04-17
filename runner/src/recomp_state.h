#pragma once
#include "types.h"

// RecompHwState: Recomp-owned hardware state.
// This struct owns all state the recomp path needs from the SNES hardware,
// making the contract explicit and decoupling from the emulator's internal
// structs (Cpu, Snes).
//
// For now this is synced from g_cpu/g_snes at frame boundaries.
// Later phases will migrate code to read/write g_recomp directly.
typedef struct RecompHwState {
  // CPU registers (for PatchBugs hooks, snapshots, debug server)
  uint16 a, x, y, sp, dp, pc;
  uint8 k, db, flags;
  bool e;       // emulation mode (should always be false for recomp)
  bool mf, xf;  // m/x width flags

  // IRQ/timing (currently sourced from g_snes->vIrqEnabled/vTimer)
  bool vIrqEnabled;
  uint16 vTimer;

  // Joypad input (currently sourced from g_snes->input1/2_currentState)
  uint16 input1, input2;

  // WRAM access port state (for registers 0x2180-0x2183)
  uint32 wramAddr;

  // Frame counter
  int frame_counter;
} RecompHwState;

extern RecompHwState g_recomp;

// Sync g_recomp from emulator state (g_cpu, g_snes).
// Call at frame boundaries or before code that reads g_recomp.
void recomp_sync_from_emu(void);

// Write g_recomp back to emulator state (g_cpu, g_snes).
// Call after code that modifies g_recomp (e.g., PatchBugs hooks).
void recomp_sync_to_emu(void);
