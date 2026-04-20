#include "recomp_state.h"
#include "snes/cpu.h"
#include "snes/snes.h"

RecompHwState g_recomp;

extern Snes *g_snes;
extern Cpu *g_cpu;
extern int snes_frame_counter;

void recomp_sync_from_emu(void) {
  g_recomp.a = g_cpu->a;
  g_recomp.x = g_cpu->x;
  g_recomp.y = g_cpu->y;
  g_recomp.sp = g_cpu->sp;
  g_recomp.dp = g_cpu->dp;
  g_recomp.pc = g_cpu->pc;
  g_recomp.k = g_cpu->k;
  g_recomp.db = g_cpu->db;
  g_recomp.flags = cpu_getFlags(g_cpu);
  g_recomp.e = g_cpu->e;
  g_recomp.mf = g_cpu->mf;
  g_recomp.xf = g_cpu->xf;

  g_recomp.vIrqEnabled = g_snes->vIrqEnabled;
  g_recomp.vTimer = g_snes->vTimer;
  g_recomp.input1 = g_snes->input1_currentState;
  g_recomp.input2 = g_snes->input2_currentState;
  g_recomp.wramAddr = g_snes->ramAdr;
  g_recomp.frame_counter = snes_frame_counter;
}
