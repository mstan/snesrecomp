/*
 * interp816 — 65816 interpreter core (the interpreter-fallback tier).
 *
 * Vendored from LakeSnes (https://github.com/angelo-wf/lakesnes), MIT,
 * Copyright (c) 2021-2023 angelo_wf and contributors. See
 * THIRD_PARTY_ATTRIBUTION.md for the full notice.
 *
 * snesrecomp adaptation:
 *   - namespaced to Interp816 / interp816_ so it coexists with the legacy
 *     `Cpu` debug shadow (runner/src/snes/cpu.{c,h}) without symbol clashes;
 *   - memory access goes through a caller-supplied callback bus, so the
 *     production adapter can point it at the AOT cpu_read8 / cpu_write8 HLE
 *     bus — one memory map, zero divergence (see docs/MULTI_TIER.md);
 *   - the snesrecomp debug instrumentation (pc_hist / DumpCpuHistory / the
 *     top-of-doOpcode assert tripwire) was stripped;
 *   - BRK dispatches to interp816_opcode_hook(), the interp<->AOT bridge
 *     seam wired in Phase 1.
 */
#ifndef INTERP816_H
#define INTERP816_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "saveload.h"

typedef struct Interp816 Interp816;

/* Caller-supplied memory bus. `mem` is opaque to the core. */
typedef uint8_t (*Interp816ReadHandler)(void *mem, uint32_t adr);
typedef void    (*Interp816WriteHandler)(void *mem, uint32_t adr, uint8_t val);

/* Optional word-width bus hooks. The core's word accesses are two byte
 * accesses (hardware bus order); a 16-bit access to a HW register then
 * releases the bus between the bytes, which a concurrently-cycled APU (the
 * runner's audio thread) can observe — on silicon the two bus cycles are
 * 6-8 master clocks apart, inside ONE SPC cycle, i.e. atomic. A handler may
 * claim a (adrl, adrh) pair and perform it through a width-preserving path
 * (the AOT bus's ReadRegWord/WriteRegWord protections); return false to fall
 * back to the two byte accesses (the handler MUST NOT have performed any
 * access in that case). `reversed` mirrors interp816_writeWord (RMW
 * write-back order, high byte first). */
typedef bool (*Interp816ReadWordHandler)(void *mem, uint32_t adrl, uint32_t adrh,
                                         uint16_t *out);
typedef bool (*Interp816WriteWordHandler)(void *mem, uint32_t adrl, uint32_t adrh,
                                          uint16_t val, bool reversed);

struct Interp816 {
  /* memory bus */
  void *mem;
  Interp816ReadHandler  read;
  Interp816WriteHandler write;
  /* optional word bus (NULL => byte-pair behavior, bit-for-bit as before) */
  Interp816ReadWordHandler  read_word;
  Interp816WriteWordHandler write_word;
  /* registers (saved block begins at `a`) */
  uint16_t a;
  uint16_t x;
  uint16_t y;
  uint16_t sp;
  uint16_t pc;
  uint16_t dp;  /* direct page (D) */
  uint8_t  k;   /* program bank (PB) */
  uint8_t  db;  /* data bank (DBR) */
  /* flags */
  bool c, z, v, n, i, d, xf, mf, e;
  /* interrupts */
  bool irqWanted, nmiWanted;
  /* power state (WAI / STP) */
  bool waiting, stopped;
  /* internal: cycles consumed by the last opcode (saved block ends here) */
  uint8_t cyclesUsed;
};

Interp816 *interp816_init(void *mem, Interp816ReadHandler read,
                          Interp816WriteHandler write);
void     interp816_free(Interp816 *cpu);
void     interp816_reset(Interp816 *cpu);
int      interp816_runOpcode(Interp816 *cpu);   /* runs one opcode; returns cycles */
uint8_t  interp816_getFlags(Interp816 *cpu);
void     interp816_setFlags(Interp816 *cpu, uint8_t val);
void     interp816_saveload(Interp816 *cpu, SaveLoadInfo *sli);

/*
 * Bridge seam. The BRK opcode dispatches here with the address of the BRK
 * byte. Return codes (preserved from the original snesrecomp HLE trap):
 *   0 = continue (BRK handled, no further action)
 *   1 = treat as RTS (pull PC, +1)
 *   2 = treat as RTL (pull PC + PB, +1)
 *   other = re-dispatch the returned value as an opcode
 * Phase 1 implements the real interp<->AOT bridge here; standalone callers
 * (e.g. the validation harness) provide a stub returning 0.
 */
extern int interp816_opcode_hook(uint32_t addr);

#endif /* INTERP816_H */
