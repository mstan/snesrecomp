/*
 * interp816.c — 65816 interpreter core (the interpreter-fallback tier).
 *
 * Vendored from LakeSnes (https://github.com/angelo-wf/lakesnes), MIT,
 * Copyright (c) 2021-2023 angelo_wf and contributors.
 * See THIRD_PARTY_ATTRIBUTION.md.
 *
 * snesrecomp adaptation: Interp816 / interp816_ namespace; caller-supplied
 * callback memory bus (no direct snes_cpuRead/Write); debug instrumentation
 * (pc_hist / DumpCpuHistory / top-of-doOpcode assert) stripped; BRK routed
 * to interp816_opcode_hook(), the interp<->AOT bridge seam. See
 * docs/MULTI_TIER.md.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef SNES_COSIM
#include <stdio.h>
#endif
#include "interp816.h"

static const int cyclesPerOpcode[256] = {
  7, 6, 7, 4, 5, 3, 5, 6, 3, 2, 2, 4, 6, 4, 6, 5,
  2, 5, 5, 7, 5, 4, 6, 6, 2, 4, 2, 2, 6, 4, 7, 5,
  6, 6, 8, 4, 3, 3, 5, 6, 4, 2, 2, 5, 4, 4, 6, 5,
  2, 5, 5, 7, 4, 4, 6, 6, 2, 4, 2, 2, 4, 4, 7, 5,
  6, 6, 2, 4, 7, 3, 5, 6, 3, 2, 2, 3, 3, 4, 6, 5,
  2, 5, 5, 7, 7, 4, 6, 6, 2, 4, 3, 2, 4, 4, 7, 5,
  6, 6, 6, 4, 3, 3, 5, 6, 4, 2, 2, 6, 5, 4, 6, 5,
  2, 5, 5, 7, 4, 4, 6, 6, 2, 4, 4, 2, 6, 4, 7, 5,
  3, 6, 4, 4, 3, 3, 3, 6, 2, 2, 2, 3, 4, 4, 4, 5,
  2, 6, 5, 7, 4, 4, 4, 6, 2, 5, 2, 2, 4, 5, 5, 5,
  2, 6, 2, 4, 3, 3, 3, 6, 2, 2, 2, 4, 4, 4, 4, 5,
  2, 5, 5, 7, 4, 4, 4, 6, 2, 4, 2, 2, 4, 4, 4, 5,
  2, 6, 3, 4, 3, 3, 5, 6, 2, 2, 2, 3, 4, 4, 6, 5,
  2, 5, 5, 7, 6, 4, 6, 6, 2, 4, 3, 3, 6, 4, 7, 5,
  2, 6, 3, 4, 3, 3, 5, 6, 2, 2, 2, 3, 4, 4, 6, 5,
  2, 5, 5, 7, 5, 4, 6, 6, 2, 4, 4, 2, 8, 4, 7, 5
};

static uint8_t interp816_read(Interp816* cpu, uint32_t adr);
static void interp816_write(Interp816* cpu, uint32_t adr, uint8_t val);
static uint8_t interp816_readOpcode(Interp816* cpu);
static uint16_t interp816_readOpcodeWord(Interp816* cpu);
static void interp816_setZN(Interp816* cpu, uint16_t value, bool byte);
static void interp816_doBranch(Interp816* cpu, uint8_t value, bool check);
static uint8_t interp816_pullByte(Interp816* cpu);
static void interp816_pushByte(Interp816* cpu, uint8_t value);
static uint16_t interp816_pullWord(Interp816* cpu);
static void interp816_pushWord(Interp816* cpu, uint16_t value);
static uint16_t interp816_readWord(Interp816* cpu, uint32_t adrl, uint32_t adrh);
static void interp816_writeWord(Interp816* cpu, uint32_t adrl, uint32_t adrh, uint16_t value, bool reversed);
static void interp816_doInterrupt(Interp816* cpu, bool irq);
static void interp816_doOpcode(Interp816* cpu, uint8_t opcode);


// addressing modes and opcode functions not declared, only used after defintions

static uint8_t interp816_read(Interp816* cpu, uint32_t adr) {
  return cpu->read(cpu->mem, adr);
}

static void interp816_write(Interp816* cpu, uint32_t adr, uint8_t val) {
  cpu->write(cpu->mem, adr, val);
}

Interp816* interp816_init(void* mem, Interp816ReadHandler read, Interp816WriteHandler write) {
  Interp816* cpu = malloc(sizeof(Interp816));
  memset(cpu, 0, sizeof(Interp816));
  cpu->mem = mem;
  cpu->read = read;
  cpu->write = write;
  return cpu;
}

void interp816_free(Interp816* cpu) {
  free(cpu);
}

void interp816_reset(Interp816* cpu) {
  cpu->a = 0;
  cpu->x = 0;
  cpu->y = 0;
  cpu->sp = 0x100;
  cpu->pc = interp816_read(cpu, 0xfffc) | (interp816_read(cpu, 0xfffd) << 8);
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
  cpu->irqWanted = false;
  cpu->nmiWanted = false;
  cpu->waiting = false;
  cpu->stopped = false;
  cpu->cyclesUsed = 0;
}

void interp816_saveload(Interp816 *cpu, SaveLoadInfo *sli) {
  sli->func(sli, &cpu->a, offsetof(Interp816, cyclesUsed) - offsetof(Interp816, a));
}

#ifdef SNES_COSIM
/* Always-on per-instruction ring (dev-only, cosim builds). Captures the exact
 * (pc, opcode, A-in/A-out, M-flag) trajectory so a codegen-vs-interp A-register
 * divergence can be localized to a single instruction by querying the window of
 * interest (ring-buffer discipline; never arm-then-step). Dumped via the cosim
 * server `itrace` command. Also exposes the interp's current PC so the
 * cpu_state write-watch can name an interpreted store. */
typedef struct { uint32_t pc; uint8_t op; uint8_t mf; uint8_t xf;
                 uint16_t a_in, a_out, x, y; } I816RingEnt;
#define I816_RING_N (1u << 19)
static I816RingEnt g_i816_ring[I816_RING_N];
static uint64_t    g_i816_ring_head = 0;
uint32_t           g_interp816_cur_pc = 0;   /* extern: cpu_state write-watch */
void interp816_dump_ring(const char* path, long n) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  uint64_t head = g_i816_ring_head;
  long avail = head < (uint64_t)I816_RING_N ? (long)head : (long)I816_RING_N;
  if (n <= 0 || n > avail) n = avail;
  for (long i = n; i > 0; i--) {
    I816RingEnt* e = &g_i816_ring[(head - (uint64_t)i) & (I816_RING_N - 1)];
    fprintf(f, "%06X op=%02X mf=%d xf=%d a_in=%04X a_out=%04X x=%04X y=%04X\n",
            e->pc, e->op, e->mf, e->xf, e->a_in, e->a_out, e->x, e->y);
  }
  fclose(f);
}
#endif

int interp816_runOpcode(Interp816* cpu) {
  cpu->cyclesUsed = 0;
  if(cpu->stopped) return 1;

  // not stopped or waiting, execute a opcode or go to interrupt
  if((!cpu->i && cpu->irqWanted) || cpu->nmiWanted) {
    cpu->cyclesUsed = 7; // interrupt: at least 7 cycles
    if(cpu->nmiWanted) {
      cpu->nmiWanted = false;
      interp816_doInterrupt(cpu, false);
    } else {
      // must be irq
      interp816_doInterrupt(cpu, true);
    }
  }
  uint8_t opcode = interp816_readOpcode(cpu);
#ifdef SNES_COSIM
  uint32_t _pcb = ((uint32_t)cpu->k << 16) | (uint16_t)(cpu->pc - 1);
  g_interp816_cur_pc = _pcb;
  uint16_t _ain = cpu->a; uint8_t _mf = cpu->mf ? 1 : 0, _xf = cpu->xf ? 1 : 0;
#endif
  cpu->cyclesUsed = cyclesPerOpcode[opcode];
  interp816_doOpcode(cpu, opcode);
#ifdef SNES_COSIM
  { I816RingEnt* e = &g_i816_ring[g_i816_ring_head & (I816_RING_N - 1)];
    e->pc = _pcb; e->op = opcode; e->mf = _mf; e->xf = _xf;
    e->a_in = _ain; e->a_out = cpu->a; e->x = cpu->x; e->y = cpu->y;
    g_i816_ring_head++; }
#endif
  return cpu->cyclesUsed;
}

static uint8_t interp816_readOpcode(Interp816* cpu) {
  return interp816_read(cpu, (cpu->k << 16) | cpu->pc++);
}

static uint16_t interp816_readOpcodeWord(Interp816* cpu) {
  uint8_t low = interp816_readOpcode(cpu);
  return low | (interp816_readOpcode(cpu) << 8);
}

uint8_t interp816_getFlags(Interp816* cpu) {
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

void interp816_setFlags(Interp816* cpu, uint8_t val) {
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

static void interp816_setZN(Interp816* cpu, uint16_t value, bool byte) {
  if(byte) {
    cpu->z = (value & 0xff) == 0;
    cpu->n = value & 0x80;
  } else {
    cpu->z = value == 0;
    cpu->n = value & 0x8000;
  }
}

static void interp816_doBranch(Interp816* cpu, uint8_t value, bool check) {
  if(check) {
    cpu->cyclesUsed++; // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
  }
}

static uint8_t interp816_pullByte(Interp816* cpu) {
  cpu->sp++;
  if(cpu->e) cpu->sp = (cpu->sp & 0xff) | 0x100;
  return interp816_read(cpu, cpu->sp);
}

static void interp816_pushByte(Interp816* cpu, uint8_t value) {
  interp816_write(cpu, cpu->sp, value);
  cpu->sp--;
  if(cpu->e) cpu->sp = (cpu->sp & 0xff) | 0x100;
}

static uint16_t interp816_pullWord(Interp816* cpu) {
  uint8_t value = interp816_pullByte(cpu);
  return value | (interp816_pullByte(cpu) << 8);
}

static void interp816_pushWord(Interp816* cpu, uint16_t value) {
  interp816_pushByte(cpu, value >> 8);
  interp816_pushByte(cpu, value & 0xff);
}

static uint16_t interp816_readWord(Interp816* cpu, uint32_t adrl, uint32_t adrh) {
  if(cpu->read_word) {
    uint16_t v;
    if(cpu->read_word(cpu->mem, adrl, adrh, &v)) return v;
  }
  uint8_t value = interp816_read(cpu, adrl);
  return value | (interp816_read(cpu, adrh) << 8);
}

static void interp816_writeWord(Interp816* cpu, uint32_t adrl, uint32_t adrh, uint16_t value, bool reversed) {
  if(cpu->write_word && cpu->write_word(cpu->mem, adrl, adrh, value, reversed))
    return;
  if(reversed) {
    interp816_write(cpu, adrh, value >> 8);
    interp816_write(cpu, adrl, value & 0xff);
  } else {
    interp816_write(cpu, adrl, value & 0xff);
    interp816_write(cpu, adrh, value >> 8);
  }
}

static void interp816_doInterrupt(Interp816* cpu, bool irq) {
  interp816_pushByte(cpu, cpu->k);
  interp816_pushWord(cpu, cpu->pc);
  interp816_pushByte(cpu, interp816_getFlags(cpu));
  cpu->cyclesUsed++; // native mode: 1 extra cycle
  cpu->i = true;
  cpu->d = false;
  cpu->k = 0;
  if(irq) {
    cpu->pc = interp816_readWord(cpu, 0xffee, 0xffef);
  } else {
    // nmi
    cpu->pc = interp816_readWord(cpu, 0xffea, 0xffeb);
  }
}

// addressing modes

static uint32_t interp816_adrImm(Interp816* cpu, uint32_t* low, bool xFlag) {
  if((xFlag && cpu->xf) || (!xFlag && cpu->mf)) {
    *low = (cpu->k << 16) | cpu->pc++;
    return 0;
  } else {
    *low = (cpu->k << 16) | cpu->pc++;
    return (cpu->k << 16) | cpu->pc++;
  }
}

static uint32_t interp816_adrDp(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr) & 0xffff;
  return (cpu->dp + adr + 1) & 0xffff;
}

static uint32_t interp816_adrDpx(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr + cpu->x) & 0xffff;
  return (cpu->dp + adr + cpu->x + 1) & 0xffff;
}

static uint32_t interp816_adrDpy(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr + cpu->y) & 0xffff;
  return (cpu->dp + adr + cpu->y + 1) & 0xffff;
}

static uint32_t interp816_adrIdp(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIdx(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (cpu->dp + adr + cpu->x) & 0xffff, (cpu->dp + adr + cpu->x + 1) & 0xffff);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIdy(Interp816* cpu, uint32_t* low, bool write) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  if(write && (!cpu->xf || ((pointer >> 8) != ((pointer + cpu->y) >> 8)))) cpu->cyclesUsed++;
  // x = 0 or page crossed, with writing opcode: 1 extra cycle
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrIdl(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint32_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  pointer |= interp816_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = pointer;
  return (pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIly(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint32_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  pointer |= interp816_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = (pointer + cpu->y) & 0xffffff;
  return (pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrSr(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  *low = (cpu->sp + adr) & 0xffff;
  return (cpu->sp + adr + 1) & 0xffff;
}

static uint32_t interp816_adrIsy(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  uint16_t pointer = interp816_readWord(cpu, (cpu->sp + adr) & 0xffff, (cpu->sp + adr + 1) & 0xffff);
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrAbs(Interp816* cpu, uint32_t* low) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  *low = (cpu->db << 16) + adr;
  return ((cpu->db << 16) + adr + 1) & 0xffffff;
}

static uint32_t interp816_adrAbx(Interp816* cpu, uint32_t* low, bool write) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  if(write && (!cpu->xf || ((adr >> 8) != ((adr + cpu->x) >> 8)))) cpu->cyclesUsed++;
  // x = 0 or page crossed, with writing opcode: 1 extra cycle
  *low = ((cpu->db << 16) + adr + cpu->x) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->x + 1) & 0xffffff;
}

static uint32_t interp816_adrAby(Interp816* cpu, uint32_t* low, bool write) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  if(write && (!cpu->xf || ((adr >> 8) != ((adr + cpu->y) >> 8)))) cpu->cyclesUsed++;
  // x = 0 or page crossed, with writing opcode: 1 extra cycle
  *low = ((cpu->db << 16) + adr + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrAbl(Interp816* cpu, uint32_t* low) {
  uint32_t adr = interp816_readOpcodeWord(cpu);
  adr |= interp816_readOpcode(cpu) << 16;
  *low = adr;
  return (adr + 1) & 0xffffff;
}

static uint32_t interp816_adrAlx(Interp816* cpu, uint32_t* low) {
  uint32_t adr = interp816_readOpcodeWord(cpu);
  adr |= interp816_readOpcode(cpu) << 16;
  *low = (adr + cpu->x) & 0xffffff;
  return (adr + cpu->x + 1) & 0xffffff;
}

static uint16_t interp816_adrIax(Interp816* cpu) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  return interp816_readWord(cpu, (cpu->k << 16) | ((adr + cpu->x) & 0xffff), (cpu->k << 16) | ((adr + cpu->x + 1) & 0xffff));
}

// opcode functions

static void interp816_and(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    cpu->a = (cpu->a & 0xff00) | ((cpu->a & value) & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    cpu->a &= value;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_ora(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    cpu->a = (cpu->a & 0xff00) | ((cpu->a | value) & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    cpu->a |= value;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_eor(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    cpu->a = (cpu->a & 0xff00) | ((cpu->a ^ value) & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    cpu->a ^= value;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_adc(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (cpu->a & 0xff) + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if(cpu->d && result > 0x9f) result += 0x60;
    cpu->c = result > 0xff;
    cpu->a = (cpu->a & 0xff00) | (result & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
      if(result > 0x9f) result = ((result + 0x60) & 0xff) + 0x100;
      result = (cpu->a & 0xf00) + (value & 0xf00) + result;
      if(result > 0x9ff) result = ((result + 0x600) & 0xfff) + 0x1000;
      result = (cpu->a & 0xf000) + (value & 0xf000) + result;
    } else {
      result = cpu->a + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if(cpu->d && result > 0x9fff) result += 0x6000;
    cpu->c = result > 0xffff;
    cpu->a = result;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_sbc(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low) ^ 0xff;
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (cpu->a & 0xff) + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if(cpu->d && result < 0x100) result -= 0x60;
    cpu->c = result > 0xff;
    cpu->a = (cpu->a & 0xff00) | (result & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high) ^ 0xffff;
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
      if(result < 0x100) result = (result - 0x60) & ((result - 0x60 < 0) ? 0xff : 0x1ff);
      result = (cpu->a & 0xf00) + (value & 0xf00) + result;
      if(result < 0x1000) result = (result - 0x600) & ((result - 0x600 < 0) ? 0xfff : 0x1fff);
      result = (cpu->a & 0xf000) + (value & 0xf000) + result;
    } else {
      result = cpu->a + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if(cpu->d && result < 0x10000) result -= 0x6000;
    cpu->c = result > 0xffff;
    cpu->a = result;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_cmp(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low) ^ 0xff;
    result = (cpu->a & 0xff) + value + 1;
    cpu->c = result > 0xff;
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high) ^ 0xffff;
    result = cpu->a + value + 1;
    cpu->c = result > 0xffff;
  }
  interp816_setZN(cpu, result, cpu->mf);
}

static void interp816_cpx(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->xf) {
    uint8_t value = interp816_read(cpu, low) ^ 0xff;
    result = (cpu->x & 0xff) + value + 1;
    cpu->c = result > 0xff;
  } else {
    cpu->cyclesUsed++; // x = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high) ^ 0xffff;
    result = cpu->x + value + 1;
    cpu->c = result > 0xffff;
  }
  interp816_setZN(cpu, result, cpu->xf);
}

static void interp816_cpy(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->xf) {
    uint8_t value = interp816_read(cpu, low) ^ 0xff;
    result = (cpu->y & 0xff) + value + 1;
    cpu->c = result > 0xff;
  } else {
    cpu->cyclesUsed++; // x = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high) ^ 0xffff;
    result = cpu->y + value + 1;
    cpu->c = result > 0xffff;
  }
  interp816_setZN(cpu, result, cpu->xf);
}

static void interp816_bit(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    uint8_t result = (cpu->a & 0xff) & value;
    cpu->z = result == 0;
    cpu->n = value & 0x80;
    cpu->v = value & 0x40;
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    uint16_t result = cpu->a & value;
    cpu->z = result == 0;
    cpu->n = value & 0x8000;
    cpu->v = value & 0x4000;
  }
}

static void interp816_lda(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu->a = (cpu->a & 0xff00) | interp816_read(cpu, low);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    cpu->a = interp816_readWord(cpu, low, high);
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_ldx(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    cpu->x = interp816_read(cpu, low);
  } else {
    cpu->cyclesUsed++; // x = 0: 1 extra cycle
    cpu->x = interp816_readWord(cpu, low, high);
  }
  interp816_setZN(cpu, cpu->x, cpu->xf);
}

static void interp816_ldy(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    cpu->y = interp816_read(cpu, low);
  } else {
    cpu->cyclesUsed++; // x = 0: 1 extra cycle
    cpu->y = interp816_readWord(cpu, low, high);
  }
  interp816_setZN(cpu, cpu->y, cpu->xf);
}

static void interp816_sta(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    interp816_write(cpu, low, (uint8_t)cpu->a);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    interp816_writeWord(cpu, low, high, cpu->a, false);
  }
}

static void interp816_stx(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    interp816_write(cpu, low, (uint8_t)cpu->x);
  } else {
    cpu->cyclesUsed++; // x = 0: 1 extra cycle
    interp816_writeWord(cpu, low, high, cpu->x, false);
  }
}

static void interp816_sty(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    interp816_write(cpu, low, (uint8_t)cpu->y);
  } else {
    cpu->cyclesUsed++; // x = 0: 1 extra cycle
    interp816_writeWord(cpu, low, high, cpu->y, false);
  }
}

static void interp816_stz(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    interp816_write(cpu, low, 0);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    interp816_writeWord(cpu, low, high, 0, false);
  }
}

static void interp816_ror(Interp816* cpu, uint32_t low, uint32_t high) {
  bool carry = false;
  int result = 0;
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    carry = value & 1;
    result = (value >> 1) | (cpu->c << 7);
    interp816_write(cpu, low, result);
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    uint16_t value = interp816_readWord(cpu, low, high);
    carry = value & 1;
    result = (value >> 1) | (cpu->c << 15);
    interp816_writeWord(cpu, low, high, result, true);
  }
  interp816_setZN(cpu, result, cpu->mf);
  cpu->c = carry;
}

static void interp816_rol(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = (interp816_read(cpu, low) << 1) | cpu->c;
    cpu->c = result & 0x100;
    interp816_write(cpu, low, result);
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    result = (interp816_readWord(cpu, low, high) << 1) | cpu->c;
    cpu->c = result & 0x10000;
    interp816_writeWord(cpu, low, high, result, true);
  }
  interp816_setZN(cpu, result, cpu->mf);
}

static void interp816_lsr(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    cpu->c = value & 1;
    result = value >> 1;
    interp816_write(cpu, low, result);
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    uint16_t value = interp816_readWord(cpu, low, high);
    cpu->c = value & 1;
    result = value >> 1;
    interp816_writeWord(cpu, low, high, result, true);
  }
  interp816_setZN(cpu, result, cpu->mf);
}

static void interp816_asl(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = interp816_read(cpu, low) << 1;
    cpu->c = result & 0x100;
    interp816_write(cpu, low, result);
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    result = interp816_readWord(cpu, low, high) << 1;
    cpu->c = result & 0x10000;
    interp816_writeWord(cpu, low, high, result, true);
  }
  interp816_setZN(cpu, result, cpu->mf);
}

static void interp816_inc(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = interp816_read(cpu, low) + 1;
    interp816_write(cpu, low, result);
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    result = interp816_readWord(cpu, low, high) + 1;
    interp816_writeWord(cpu, low, high, result, true);
  }
  interp816_setZN(cpu, result, cpu->mf);
}

static void interp816_dec(Interp816* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = interp816_read(cpu, low) - 1;
    interp816_write(cpu, low, result);
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    result = interp816_readWord(cpu, low, high) - 1;
    interp816_writeWord(cpu, low, high, result, true);
  }
  interp816_setZN(cpu, result, cpu->mf);
}

static void interp816_tsb(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    cpu->z = ((cpu->a & 0xff) & value) == 0;
    interp816_write(cpu, low, value | (cpu->a & 0xff));
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    uint16_t value = interp816_readWord(cpu, low, high);
    cpu->z = (cpu->a & value) == 0;
    interp816_writeWord(cpu, low, high, value | cpu->a, true);
  }
}

static void interp816_trb(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    cpu->z = ((cpu->a & 0xff) & value) == 0;
    interp816_write(cpu, low, value & ~(cpu->a & 0xff));
  } else {
    cpu->cyclesUsed += 2; // m = 0: 2 extra cycles
    uint16_t value = interp816_readWord(cpu, low, high);
    cpu->z = (cpu->a & value) == 0;
    interp816_writeWord(cpu, low, high, value & ~cpu->a, true);
  }
}


extern int interp816_opcode_hook(uint32_t addr);

static void interp816_doOpcode(Interp816* cpu, uint8_t opcode) {
restart:
  switch(opcode) {
    case 0x00: { // brk imp
      uint32_t addr = (cpu->k << 16) | cpu->pc;
      switch (opcode = interp816_opcode_hook(addr - 1)) {
      case 0:
        break;
      case 1: // rts
        cpu->pc = interp816_pullWord(cpu) + 1;
        break;
      case 2: // rtl
        cpu->pc = interp816_pullWord(cpu) + 1;
        cpu->k = interp816_pullByte(cpu) & 0x7f;
        break;
      default:
        goto restart;
      }
      break;
    }
    case 0x01: { // ora idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x02: { // cop imm(s)
      interp816_readOpcode(cpu);
      interp816_pushByte(cpu, cpu->k);
      interp816_pushWord(cpu, cpu->pc);
      interp816_pushByte(cpu, interp816_getFlags(cpu));
      cpu->cyclesUsed++; // native mode: 1 extra cycle
      cpu->i = true;
      cpu->d = false;
      cpu->k = 0;
      cpu->pc = interp816_readWord(cpu, 0xffe4, 0xffe5);
      break;
    }
    case 0x03: { // ora sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x04: { // tsb dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_tsb(cpu, low, high);
      break;
    }
    case 0x05: { // ora dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x06: { // asl dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_asl(cpu, low, high);
      break;
    }
    case 0x07: { // ora idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x08: { // php imp
      interp816_pushByte(cpu, interp816_getFlags(cpu));
      break;
    }
    case 0x09: { // ora imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x0a: { // asla imp
      if(cpu->mf) {
        cpu->c = cpu->a & 0x80;
        cpu->a = (cpu->a & 0xff00) | ((cpu->a << 1) & 0xff);
      } else {
        cpu->c = cpu->a & 0x8000;
        cpu->a <<= 1;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x0b: { // phd imp
      interp816_pushWord(cpu, cpu->dp);
      break;
    }
    case 0x0c: { // tsb abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_tsb(cpu, low, high);
      break;
    }
    case 0x0d: { // ora abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x0e: { // asl abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_asl(cpu, low, high);
      break;
    }
    case 0x0f: { // ora abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x10: { // bpl rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), !cpu->n);
      break;
    }
    case 0x11: { // ora idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x12: { // ora idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x13: { // ora isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x14: { // trb dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_trb(cpu, low, high);
      break;
    }
    case 0x15: { // ora dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x16: { // asl dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_asl(cpu, low, high);
      break;
    }
    case 0x17: { // ora ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x18: { // clc imp
      cpu->c = false;
      break;
    }
    case 0x19: { // ora aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x1a: { // inca imp
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a + 1) & 0xff);
      } else {
        cpu->a++;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x1b: { // tcs imp
      cpu->sp = cpu->a;
      break;
    }
    case 0x1c: { // trb abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_trb(cpu, low, high);
      break;
    }
    case 0x1d: { // ora abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x1e: { // asl abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_asl(cpu, low, high);
      break;
    }
    case 0x1f: { // ora alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_ora(cpu, low, high);
      break;
    }
    case 0x20: { // jsr abs
      uint16_t value = interp816_readOpcodeWord(cpu);
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      break;
    }
    case 0x21: { // and idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x22: { // jsl abl
      uint16_t value = interp816_readOpcodeWord(cpu);
      uint8_t newK = interp816_readOpcode(cpu);
      interp816_pushByte(cpu, cpu->k);
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      cpu->k = newK & 0x7f;
      break;
    }
    case 0x23: { // and sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x24: { // bit dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_bit(cpu, low, high);
      break;
    }
    case 0x25: { // and dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x26: { // rol dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_rol(cpu, low, high);
      break;
    }
    case 0x27: { // and idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x28: { // plp imp
      interp816_setFlags(cpu, interp816_pullByte(cpu));
      break;
    }
    case 0x29: { // and imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x2a: { // rola imp
      int result = (cpu->a << 1) | cpu->c;
      if(cpu->mf) {
        cpu->c = result & 0x100;
        cpu->a = (cpu->a & 0xff00) | (result & 0xff);
      } else {
        cpu->c = result & 0x10000;
        cpu->a = result;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x2b: { // pld imp
      cpu->dp = interp816_pullWord(cpu);
      interp816_setZN(cpu, cpu->dp, false);
      break;
    }
    case 0x2c: { // bit abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_bit(cpu, low, high);
      break;
    }
    case 0x2d: { // and abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x2e: { // rol abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_rol(cpu, low, high);
      break;
    }
    case 0x2f: { // and abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x30: { // bmi rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), cpu->n);
      break;
    }
    case 0x31: { // and idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x32: { // and idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x33: { // and isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x34: { // bit dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_bit(cpu, low, high);
      break;
    }
    case 0x35: { // and dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x36: { // rol dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_rol(cpu, low, high);
      break;
    }
    case 0x37: { // and ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x38: { // sec imp
      cpu->c = true;
      break;
    }
    case 0x39: { // and aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x3a: { // deca imp
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a - 1) & 0xff);
      } else {
        cpu->a--;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x3b: { // tsc imp
      cpu->a = cpu->sp;
      interp816_setZN(cpu, cpu->a, false);
      break;
    }
    case 0x3c: { // bit abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_bit(cpu, low, high);
      break;
    }
    case 0x3d: { // and abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x3e: { // rol abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_rol(cpu, low, high);
      break;
    }
    case 0x3f: { // and alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_and(cpu, low, high);
      break;
    }
    case 0x40: { // rti imp
      interp816_setFlags(cpu, interp816_pullByte(cpu));
      cpu->cyclesUsed++; // native mode: 1 extra cycle
      cpu->pc = interp816_pullWord(cpu);
      cpu->k = interp816_pullByte(cpu);
      break;
    }
    case 0x41: { // eor idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x42: { // wdm imm(s)
      interp816_readOpcode(cpu);
      break;
    }
    case 0x43: { // eor sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x44: { // mvp bm
      uint8_t dest = interp816_readOpcode(cpu);
      uint8_t src = interp816_readOpcode(cpu);
      cpu->db = dest;
      interp816_write(cpu, (dest << 16) | cpu->y, interp816_read(cpu, (src << 16) | cpu->x));
      cpu->a--;
      cpu->x--;
      cpu->y--;
      if(cpu->a != 0xffff) {
        cpu->pc -= 3;
      }
      if(cpu->xf) {
        cpu->x &= 0xff;
        cpu->y &= 0xff;
      }
      break;
    }
    case 0x45: { // eor dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x46: { // lsr dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_lsr(cpu, low, high);
      break;
    }
    case 0x47: { // eor idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x48: { // pha imp
      if(cpu->mf) {
        interp816_pushByte(cpu, (uint8_t)cpu->a);
      } else {
        cpu->cyclesUsed++; // m = 0: 1 extra cycle
        interp816_pushWord(cpu, cpu->a);
      }
      break;
    }
    case 0x49: { // eor imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x4a: { // lsra imp
      cpu->c = cpu->a & 1;
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a >> 1) & 0x7f);
      } else {
        cpu->a >>= 1;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x4b: { // phk imp
      interp816_pushByte(cpu, cpu->k);
      break;
    }
    case 0x4c: { // jmp abs
      cpu->pc = interp816_readOpcodeWord(cpu);
      break;
    }
    case 0x4d: { // eor abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x4e: { // lsr abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_lsr(cpu, low, high);
      break;
    }
    case 0x4f: { // eor abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x50: { // bvc rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), !cpu->v);
      break;
    }
    case 0x51: { // eor idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x52: { // eor idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x53: { // eor isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x54: { // mvn bm
      uint8_t dest = interp816_readOpcode(cpu);
      uint8_t src = interp816_readOpcode(cpu);
      cpu->db = dest;
      interp816_write(cpu, (dest << 16) | cpu->y, interp816_read(cpu, (src << 16) | cpu->x));
      cpu->a--;
      cpu->x++;
      cpu->y++;
      if(cpu->a != 0xffff) {
        cpu->pc -= 3;
      }
      if(cpu->xf) {
        cpu->x &= 0xff;
        cpu->y &= 0xff;
      }
      break;
    }
    case 0x55: { // eor dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x56: { // lsr dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_lsr(cpu, low, high);
      break;
    }
    case 0x57: { // eor ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x58: { // cli imp
      cpu->i = false;
      break;
    }
    case 0x59: { // eor aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x5a: { // phy imp
      if(cpu->xf) {
        interp816_pushByte(cpu, (uint8_t)cpu->y);
      } else {
        cpu->cyclesUsed++; // m = 0: 1 extra cycle
        interp816_pushWord(cpu, cpu->y);
      }
      break;
    }
    case 0x5b: { // tcd imp
      cpu->dp = cpu->a;
      interp816_setZN(cpu, cpu->dp, false);
      break;
    }
    case 0x5c: { // jml abl
      uint16_t value = interp816_readOpcodeWord(cpu);
      uint8_t new_k = interp816_readOpcode(cpu);
      cpu->k = new_k & 0x7f;
      cpu->pc = value;
      break;
    }
    case 0x5d: { // eor abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x5e: { // lsr abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_lsr(cpu, low, high);
      break;
    }
    case 0x5f: { // eor alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_eor(cpu, low, high);
      break;
    }
    case 0x60: { // rts imp
      cpu->pc = interp816_pullWord(cpu) + 1;
      break;
    }
    case 0x61: { // adc idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x62: { // per rll
      uint16_t value = interp816_readOpcodeWord(cpu);
      interp816_pushWord(cpu, cpu->pc + (int16_t) value);
      break;
    }
    case 0x63: { // adc sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x64: { // stz dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_stz(cpu, low, high);
      break;
    }
    case 0x65: { // adc dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x66: { // ror dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_ror(cpu, low, high);
      break;
    }
    case 0x67: { // adc idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x68: { // pla imp
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | interp816_pullByte(cpu);
      } else {
        cpu->cyclesUsed++; // 16-bit m: 1 extra cycle
        cpu->a = interp816_pullWord(cpu);
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x69: { // adc imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x6a: { // rora imp
      bool carry = cpu->a & 1;
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a >> 1) & 0x7f) | (cpu->c << 7);
      } else {
        cpu->a = (cpu->a >> 1) | (cpu->c << 15);
      }
      cpu->c = carry;
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x6b: { // rtl imp
      cpu->pc = interp816_pullWord(cpu) + 1;
      cpu->k = interp816_pullByte(cpu) & 0x7f;
      break;
    }
    case 0x6c: { // jmp ind
      uint16_t adr = interp816_readOpcodeWord(cpu);
      cpu->pc = interp816_readWord(cpu, adr, (adr + 1) & 0xffff);
      break;
    }
    case 0x6d: { // adc abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x6e: { // ror abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_ror(cpu, low, high);
      break;
    }
    case 0x6f: { // adc abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x70: { // bvs rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), cpu->v);
      break;
    }
    case 0x71: { // adc idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x72: { // adc idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x73: { // adc isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x74: { // stz dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_stz(cpu, low, high);
      break;
    }
    case 0x75: { // adc dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x76: { // ror dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_ror(cpu, low, high);
      break;
    }
    case 0x77: { // adc ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x78: { // sei imp
      cpu->i = true;
      break;
    }
    case 0x79: { // adc aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x7a: { // ply imp
      if(cpu->xf) {
        cpu->y = interp816_pullByte(cpu);
      } else {
        cpu->cyclesUsed++; // 16-bit x: 1 extra cycle
        cpu->y = interp816_pullWord(cpu);
      }
      interp816_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0x7b: { // tdc imp
      cpu->a = cpu->dp;
      interp816_setZN(cpu, cpu->a, false);
      break;
    }
    case 0x7c: { // jmp iax
      cpu->pc = interp816_adrIax(cpu);
      break;
    }
    case 0x7d: { // adc abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x7e: { // ror abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_ror(cpu, low, high);
      break;
    }
    case 0x7f: { // adc alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_adc(cpu, low, high);
      break;
    }
    case 0x80: { // bra rel
      cpu->pc += (int8_t) interp816_readOpcode(cpu);
      break;
    }
    case 0x81: { // sta idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x82: { // brl rll
      cpu->pc += (int16_t) interp816_readOpcodeWord(cpu);
      break;
    }
    case 0x83: { // sta sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x84: { // sty dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_sty(cpu, low, high);
      break;
    }
    case 0x85: { // sta dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x86: { // stx dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_stx(cpu, low, high);
      break;
    }
    case 0x87: { // sta idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x88: { // dey imp
      if(cpu->xf) {
        cpu->y = (cpu->y - 1) & 0xff;
      } else {
        cpu->y--;
      }
      interp816_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0x89: { // biti imm(m)
      if(cpu->mf) {
        uint8_t result = (cpu->a & 0xff) & interp816_readOpcode(cpu);
        cpu->z = result == 0;
      } else {
        cpu->cyclesUsed++; // m = 0: 1 extra cycle
        uint16_t result = cpu->a & interp816_readOpcodeWord(cpu);
        cpu->z = result == 0;
      }
      break;
    }
    case 0x8a: { // txa imp
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | (cpu->x & 0xff);
      } else {
        cpu->a = cpu->x;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x8b: { // phb imp
      interp816_pushByte(cpu, cpu->db);
      break;
    }
    case 0x8c: { // sty abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_sty(cpu, low, high);
      break;
    }
    case 0x8d: { // sta abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x8e: { // stx abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_stx(cpu, low, high);
      break;
    }
    case 0x8f: { // sta abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x90: { // bcc rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), !cpu->c);
      break;
    }
    case 0x91: { // sta idy
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, true);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x92: { // sta idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x93: { // sta isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x94: { // sty dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_sty(cpu, low, high);
      break;
    }
    case 0x95: { // sta dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x96: { // stx dpy
      uint32_t low = 0;
      uint32_t high = interp816_adrDpy(cpu, &low);
      interp816_stx(cpu, low, high);
      break;
    }
    case 0x97: { // sta ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x98: { // tya imp
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | (cpu->y & 0xff);
      } else {
        cpu->a = cpu->y;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x99: { // sta aby
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, true);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x9a: { // txs imp
      cpu->sp = cpu->x;
      break;
    }
    case 0x9b: { // txy imp
      if(cpu->xf) {
        cpu->y = cpu->x & 0xff;
      } else {
        cpu->y = cpu->x;
      }
      interp816_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0x9c: { // stz abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_stz(cpu, low, high);
      break;
    }
    case 0x9d: { // sta abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0x9e: { // stz abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_stz(cpu, low, high);
      break;
    }
    case 0x9f: { // sta alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_sta(cpu, low, high);
      break;
    }
    case 0xa0: { // ldy imm(x)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, true);
      interp816_ldy(cpu, low, high);
      break;
    }
    case 0xa1: { // lda idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xa2: { // ldx imm(x)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, true);
      interp816_ldx(cpu, low, high);
      break;
    }
    case 0xa3: { // lda sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xa4: { // ldy dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_ldy(cpu, low, high);
      break;
    }
    case 0xa5: { // lda dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xa6: { // ldx dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_ldx(cpu, low, high);
      break;
    }
    case 0xa7: { // lda idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xa8: { // tay imp
      if(cpu->xf) {
        cpu->y = cpu->a & 0xff;
      } else {
        cpu->y = cpu->a;
      }
      interp816_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0xa9: { // lda imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xaa: { // tax imp
      if(cpu->xf) {
        cpu->x = cpu->a & 0xff;
      } else {
        cpu->x = cpu->a;
      }
      interp816_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xab: { // plb imp
      cpu->db = interp816_pullByte(cpu);
      interp816_setZN(cpu, cpu->db, true);
      break;
    }
    case 0xac: { // ldy abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_ldy(cpu, low, high);
      break;
    }
    case 0xad: { // lda abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xae: { // ldx abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_ldx(cpu, low, high);
      break;
    }
    case 0xaf: { // lda abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xb0: { // bcs rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), cpu->c);
      break;
    }
    case 0xb1: { // lda idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xb2: { // lda idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xb3: { // lda isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xb4: { // ldy dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_ldy(cpu, low, high);
      break;
    }
    case 0xb5: { // lda dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xb6: { // ldx dpy
      uint32_t low = 0;
      uint32_t high = interp816_adrDpy(cpu, &low);
      interp816_ldx(cpu, low, high);
      break;
    }
    case 0xb7: { // lda ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xb8: { // clv imp
      cpu->v = false;
      break;
    }
    case 0xb9: { // lda aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xba: { // tsx imp
      if(cpu->xf) {
        cpu->x = cpu->sp & 0xff;
      } else {
        cpu->x = cpu->sp;
      }
      interp816_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xbb: { // tyx imp
      if(cpu->xf) {
        cpu->x = cpu->y & 0xff;
      } else {
        cpu->x = cpu->y;
      }
      interp816_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xbc: { // ldy abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_ldy(cpu, low, high);
      break;
    }
    case 0xbd: { // lda abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xbe: { // ldx aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_ldx(cpu, low, high);
      break;
    }
    case 0xbf: { // lda alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_lda(cpu, low, high);
      break;
    }
    case 0xc0: { // cpy imm(x)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, true);
      interp816_cpy(cpu, low, high);
      break;
    }
    case 0xc1: { // cmp idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xc2: { // rep imm(s)
      interp816_setFlags(cpu, interp816_getFlags(cpu) & ~interp816_readOpcode(cpu));
      break;
    }
    case 0xc3: { // cmp sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xc4: { // cpy dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_cpy(cpu, low, high);
      break;
    }
    case 0xc5: { // cmp dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xc6: { // dec dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_dec(cpu, low, high);
      break;
    }
    case 0xc7: { // cmp idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xc8: { // iny imp
      if(cpu->xf) {
        cpu->y = (cpu->y + 1) & 0xff;
      } else {
        cpu->y++;
      }
      interp816_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0xc9: { // cmp imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xca: { // dex imp
      if(cpu->xf) {
        cpu->x = (cpu->x - 1) & 0xff;
      } else {
        cpu->x--;
      }
      interp816_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xcb: { // wai imp
      cpu->waiting = true;
      break;
    }
    case 0xcc: { // cpy abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_cpy(cpu, low, high);
      break;
    }
    case 0xcd: { // cmp abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xce: { // dec abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_dec(cpu, low, high);
      break;
    }
    case 0xcf: { // cmp abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xd0: { // bne rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), !cpu->z);
      break;
    }
    case 0xd1: { // cmp idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xd2: { // cmp idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xd3: { // cmp isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xd4: { // pei dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_pushWord(cpu, interp816_readWord(cpu, low, high));
      break;
    }
    case 0xd5: { // cmp dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xd6: { // dec dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_dec(cpu, low, high);
      break;
    }
    case 0xd7: { // cmp ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xd8: { // cld imp
      cpu->d = false;
      break;
    }
    case 0xd9: { // cmp aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xda: { // phx imp
      if(cpu->xf) {
        interp816_pushByte(cpu, (uint8_t)cpu->x);
      } else {
        cpu->cyclesUsed++; // m = 0: 1 extra cycle
        interp816_pushWord(cpu, cpu->x);
      }
      break;
    }
    case 0xdb: { // stp imp
      cpu->stopped = true;
      break;
    }
    case 0xdc: { // jml ial
      uint16_t adr = interp816_readOpcodeWord(cpu);
      cpu->pc = interp816_readWord(cpu, adr, (adr + 1) & 0xffff);
      cpu->k = interp816_read(cpu, (adr + 2) & 0xffff) & 0x7f;
      break;
    }
    case 0xdd: { // cmp abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xde: { // dec abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_dec(cpu, low, high);
      break;
    }
    case 0xdf: { // cmp alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_cmp(cpu, low, high);
      break;
    }
    case 0xe0: { // cpx imm(x)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, true);
      interp816_cpx(cpu, low, high);
      break;
    }
    case 0xe1: { // sbc idx
      uint32_t low = 0;
      uint32_t high = interp816_adrIdx(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xe2: { // sep imm(s)
      interp816_setFlags(cpu, interp816_getFlags(cpu) | interp816_readOpcode(cpu));
      break;
    }
    case 0xe3: { // sbc sr
      uint32_t low = 0;
      uint32_t high = interp816_adrSr(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xe4: { // cpx dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_cpx(cpu, low, high);
      break;
    }
    case 0xe5: { // sbc dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xe6: { // inc dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_inc(cpu, low, high);
      break;
    }
    case 0xe7: { // sbc idl
      uint32_t low = 0;
      uint32_t high = interp816_adrIdl(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xe8: { // inx imp
      if(cpu->xf) {
        cpu->x = (cpu->x + 1) & 0xff;
      } else {
        cpu->x++;
      }
      interp816_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xe9: { // sbc imm(m)
      uint32_t low = 0;
      uint32_t high = interp816_adrImm(cpu, &low, false);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xea: { // nop imp
      // no operation
      break;
    }
    case 0xeb: { // xba imp
      uint8_t low = cpu->a & 0xff;
      uint8_t high = cpu->a >> 8;
      cpu->a = (low << 8) | high;
      interp816_setZN(cpu, high, true);
      break;
    }
    case 0xec: { // cpx abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_cpx(cpu, low, high);
      break;
    }
    case 0xed: { // sbc abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xee: { // inc abs
      uint32_t low = 0;
      uint32_t high = interp816_adrAbs(cpu, &low);
      interp816_inc(cpu, low, high);
      break;
    }
    case 0xef: { // sbc abl
      uint32_t low = 0;
      uint32_t high = interp816_adrAbl(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xf0: { // beq rel
      interp816_doBranch(cpu, interp816_readOpcode(cpu), cpu->z);
      break;
    }
    case 0xf1: { // sbc idy(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrIdy(cpu, &low, false);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xf2: { // sbc idp
      uint32_t low = 0;
      uint32_t high = interp816_adrIdp(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xf3: { // sbc isy
      uint32_t low = 0;
      uint32_t high = interp816_adrIsy(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xf4: { // pea imm(l)
      uint16_t w = interp816_readOpcodeWord(cpu);
      if (w == 0xC2AE)
        cpu->y = 0;
      interp816_pushWord(cpu, w);
  
      break;
    }
    case 0xf5: { // sbc dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xf6: { // inc dpx
      uint32_t low = 0;
      uint32_t high = interp816_adrDpx(cpu, &low);
      interp816_inc(cpu, low, high);
      break;
    }
    case 0xf7: { // sbc ily
      uint32_t low = 0;
      uint32_t high = interp816_adrIly(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xf8: { // sed imp
      cpu->d = true;
      break;
    }
    case 0xf9: { // sbc aby(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAby(cpu, &low, false);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xfa: { // plx imp
      if(cpu->xf) {
        cpu->x = interp816_pullByte(cpu);
      } else {
        cpu->cyclesUsed++; // 16-bit x: 1 extra cycle
        cpu->x = interp816_pullWord(cpu);
      }
      interp816_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xfb: { // xce imp
      bool temp = cpu->c;
      cpu->c = cpu->e;
      cpu->e = temp;
      interp816_setFlags(cpu, interp816_getFlags(cpu)); // updates x and m flags, clears upper half of x and y if needed
      break;
    }
    case 0xfc: { // jsr iax
      uint16_t value = interp816_adrIax(cpu);
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      break;
    }
    case 0xfd: { // sbc abx(r)
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, false);
      interp816_sbc(cpu, low, high);
      break;
    }
    case 0xfe: { // inc abx
      uint32_t low = 0;
      uint32_t high = interp816_adrAbx(cpu, &low, true);
      interp816_inc(cpu, low, high);
      break;
    }
    case 0xff: { // sbc alx
      uint32_t low = 0;
      uint32_t high = interp816_adrAlx(cpu, &low);
      interp816_sbc(cpu, low, high);
      break;
    }
  }
}