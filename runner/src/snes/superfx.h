#ifndef SNESRECOMP_SUPERFX_H
#define SNESRECOMP_SUPERFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SuperFxReg {
  uint16_t data;
  bool modified;
} SuperFxReg;

typedef struct SuperFxPixelCache {
  uint16_t offset;
  uint8_t bitpend;
  uint8_t data[8];
} SuperFxPixelCache;

typedef struct SuperFxTraceEntry {
  uint64_t sequence;
  uint16_t r15;
  uint16_t sfr;
  uint8_t pbr;
  uint8_t opcode;
} SuperFxTraceEntry;

/* Architectural state for the Nintendo GSU/Super FX coprocessor.  This is a
 * correctness-oriented LLE core: callers expose the real register and memory
 * buses and advance it from the shared SNES master-clock timeline. */
typedef struct SuperFx {
  uint8_t *rom;
  uint32_t rom_size, rom_mask;
  uint8_t *ram;
  uint32_t ram_size, ram_mask;

  SuperFxReg r[16];
  uint16_t sfr;
  uint8_t pbr, rombr, rambr;
  uint16_t cbr;
  uint8_t scbr, scmr, colr, por, bramr, vcr, cfgr, clsr;
  uint8_t pipeline;
  uint16_t ramaddr;
  uint8_t sreg, dreg;

  uint32_t romcl;
  uint8_t romdr;
  uint32_t ramcl;
  uint16_t ramar;
  uint8_t ramdr;

  uint8_t cache[512];
  bool cache_valid[32];
  SuperFxPixelCache pixel[2];

  uint64_t master_clock;
  int64_t clock_credit;
  bool irq_pending;

  uint64_t instruction_count;
  SuperFxTraceEntry trace[256];
} SuperFx;

SuperFx *superfx_create(uint8_t *rom, uint32_t rom_size,
                        uint8_t *ram, uint32_t ram_size);
void superfx_destroy(SuperFx *fx);
void superfx_reset(SuperFx *fx);

/* Synchronize to the S-CPU's monotonically increasing SNES master clock. */
void superfx_sync(SuperFx *fx, uint64_t master_clock);

uint8_t superfx_cpu_read_io(SuperFx *fx, uint16_t address);
void superfx_cpu_write_io(SuperFx *fx, uint16_t address, uint8_t data);
uint8_t superfx_cpu_read_rom(SuperFx *fx, uint32_t address, uint8_t open_bus);
uint8_t superfx_cpu_read_ram(SuperFx *fx, uint32_t address, uint8_t open_bus);
void superfx_cpu_write_ram(SuperFx *fx, uint32_t address, uint8_t data);

#endif
