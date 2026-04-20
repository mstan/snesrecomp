#include "recomp_state.h"
#include "snes/snes.h"

RecompHwState g_recomp;

extern Snes *g_snes;

void recomp_sync_from_emu(void) {
  g_recomp.vIrqEnabled = g_snes->vIrqEnabled;
  g_recomp.vTimer = g_snes->vTimer;
  g_recomp.input1 = g_snes->input1_currentState;
  g_recomp.input2 = g_snes->input2_currentState;
  g_recomp.wramAddr = g_snes->ramAdr;
}
