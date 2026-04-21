// 65816 CPU framework state — interpreter ripped 2026-04-20.
//
// We do not interpret SNES instructions; the recompiler emits C bodies
// that are called directly. Only init / reset / save-load / flag pack
// helpers remain — those are needed because cpu state (D, DB, PB, m/x
// flags, e bit, sp) is consulted by recompiled code and by the SNES
// emulator's other subsystems for save/load.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cpu.h"
#include "snes.h"
#include "../types.h"
#include "../common_rtl.h"
#include "variables.h"

Cpu* cpu_init(void) {
  Cpu* cpu = malloc(sizeof(Cpu));
  memset(cpu, 0, sizeof(Cpu));
  return cpu;
}

void cpu_free(Cpu* cpu) {
  free(cpu);
}

void cpu_reset(Cpu* cpu) {
  cpu->a = 0;
  cpu->x = 0;
  cpu->y = 0;
  cpu->sp = 0x100;
  cpu->pc = 0;  // recomp drives PC via direct C calls; vector load dropped
  cpu->dp = 0;
  cpu->k = 0;
  cpu->db = 0;
  cpu->c = false;
  cpu->z = false;
  cpu->v = false;
  cpu->n = false;
  cpu->i = true;
  cpu->d = false;
  cpu->xf = true;
  cpu->mf = true;
  cpu->e = true;
}

// Saves the entire Cpu struct as a single blob — registers + flags only
// after the interpreter rip; struct end == end of saved region.
void cpu_saveload(Cpu *cpu, SaveLoadInfo *sli) {
  sli->func(sli, &cpu->a, sizeof(*cpu) - offsetof(Cpu, a));
}

uint8_t cpu_getFlags(Cpu* cpu) {
  uint8_t val = cpu->n << 7;
  val |= cpu->v << 6;
  val |= cpu->mf << 5;
  val |= cpu->xf << 4;
  val |= cpu->d << 3;
  val |= cpu->i << 2;
  val |= cpu->z << 1;
  val |= cpu->c;
  return val;
}

void cpu_setFlags(Cpu* cpu, uint8_t val) {
  cpu->n = val & 0x80;
  cpu->v = val & 0x40;
  cpu->mf = val & 0x20;
  cpu->xf = val & 0x10;
  cpu->d = val & 8;
  cpu->i = val & 4;
  cpu->z = val & 2;
  cpu->c = val & 1;
  if(cpu->e) {
    cpu->mf = true;
    cpu->xf = true;
    cpu->sp = (cpu->sp & 0xff) | 0x100;
  }
  if(cpu->xf) {
    cpu->x &= 0xff;
    cpu->y &= 0xff;
  }
}
