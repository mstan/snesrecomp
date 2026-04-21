
#ifndef CPU_H
#define CPU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "saveload.h"

typedef struct Cpu Cpu;

// Interpreter ripped 2026-04-20 — Cpu now holds only the register +
// flag state that recompiled C bodies and the framework consult
// (mostly D, DB, PB, m/x flags, e bit, sp). Interpreter-only fields
// (mem/memType, irqWanted/nmiWanted, waiting/stopped, cyclesUsed,
// in_emu) were write-only-never-read after the interpreter rip.
struct Cpu {
  // registers
  uint16_t a;
  uint16_t x;
  uint16_t y;
  uint16_t sp;
  uint16_t pc;
  uint16_t dp; // direct page (D)
  uint8_t k; // program bank (PB)
  uint8_t db; // data bank (B)
  // flags
  bool c;
  bool z;
  bool v;
  bool n;
  bool i;
  bool d;
  bool xf;
  bool mf;
  bool e;
};

extern struct Cpu *g_cpu;

Cpu* cpu_init(void);
void cpu_free(Cpu* cpu);
void cpu_reset(Cpu* cpu);
uint8_t cpu_getFlags(Cpu *cpu);
void cpu_setFlags(Cpu *cpu, uint8_t val);
void cpu_saveload(Cpu *cpu, SaveLoadInfo *sli);
#endif
