#ifndef SNES_CX4_H
#define SNES_CX4_H

#include <stdint.h>

/* Capcom Cx4 — a Hitachi HG51B S169 DSP in a Capcom package.
 *
 * Used by exactly two commercial titles: Mega Man X2 and Mega Man X3. Both
 * declare it through the internal header as chipset $F3 (ROM + coprocessor,
 * coprocessor id $F "custom") with the $FFBF sub-type byte $10.
 *
 * This is INSTRUCTION-LEVEL emulation: the DSP fetches and executes the Cx4
 * program out of cartridge ROM, so it is the faithful floor per PRINCIPLES.md
 * rather than a host-side reimplementation of what that program computes.
 * There is consequently no "unknown command" surface — whatever the game's own
 * Cx4 code does, the core runs.
 *
 * Ported from ares (ISC licence) — see THIRD_PARTY_ATTRIBUTION.md.
 *
 * ── CPU-visible map (Mapping 0, which is what MMX2/X3 use) ────────────────
 *
 *   $00-$3F,$80-$BF:$6000-$6BFF   3 KB DSP data RAM  (mirrored at $7000-$7BFF)
 *   $00-$3F,$80-$BF:$6C00-$6FFF   IO window          (mirrored at $7C00-$7FFF)
 *     ...within which:
 *       $7F40-$7F47   DMA source / length / target  (target byte 2 triggers)
 *       $7F48-$7F4F   program cache: page, base, locks, entry PB/PC
 *                     $7F4F ALSO STARTS THE DSP when it is halted — the value
 *                     is the entry program counter, not a command id
 *       $7F50-$7F5F   wait states, IRQ enable, ROM config, suspend, status
 *       $7F60-$7F7F   32 vector bytes
 *       $7F80-$7FAF   general-purpose registers R0-R15, 3 bytes each
 *       $7FC0-$7FEF   mirror of the GPR window
 *   $00-$3F,$80-$BF:$8000-$FFFF   ordinary LoROM
 *   $70-$77:$0000-$7FFF           cartridge save RAM (neither X2 nor X3 has any)
 *
 * The DSP runs at 20 MHz against the SNES's ~21.477 MHz master clock, so it
 * must be advanced before any observation of its state — see cx4_sync().
 */

typedef struct Cx4 Cx4;
struct SaveLoadInfo;

/* rom/rom_size is the cartridge image in the engine's linear LoROM layout;
 * ram/ram_size is cartridge save RAM (may be NULL/0). Neither is owned. */
Cx4 *cx4_create(const uint8_t *rom, uint32_t rom_size,
                uint8_t *ram, uint32_t ram_size);
void cx4_destroy(Cx4 *cx4);
void cx4_reset(Cx4 *cx4);

/* Advance the DSP to the given SNES master clock. MUST be called before any
 * read/write of the Cx4 window and before DMA sources data out of its RAM:
 * unlike a command-level model, this DSP's results appear over time. */
void cx4_sync(Cx4 *cx4, uint64_t master_clock);

/* addr is the raw 16-bit CPU address; only $6000-$7FFF is meaningful. */
uint8_t cx4_read(Cx4 *cx4, uint16_t addr);
void cx4_write(Cx4 *cx4, uint16_t addr, uint8_t val);

/* Stable storage for bulk readers (DMA, debug dumps), or NULL when the address
 * is an IO register that must go through cx4_read/cx4_write. */
uint8_t *cx4_ram_ptr(Cx4 *cx4, uint16_t addr);

/* True while the DSP owns the bus. In that state a CPU read of the vector area
 * ($00:$7FC0-$7FFF under Mapping 0) returns Cx4 IO registers instead of ROM,
 * which is how the coprocessor overrides the reset/interrupt vectors. */
int cx4_owns_bus(const Cx4 *cx4);
uint8_t cx4_read_vector_override(Cx4 *cx4, uint16_t addr);

/* The Cx4 can assert the CPU IRQ line when it halts (if IRQs are enabled).
 * Mirrors superfx->irq_pending; consumers must OR it into their IRQ check. */
int cx4_irq_pending(const Cx4 *cx4);
void cx4_irq_acknowledge(Cx4 *cx4);

void cx4_saveload(Cx4 *cx4, struct SaveLoadInfo *sli);

/* ── data ROM ─────────────────────────────────────────────────────────────
 * The HG51B S169 carries a 1024-entry x 24-bit internal data ROM that is NOT
 * part of the game ROM. It turns out to be six closed-form mathematical tables
 * (reciprocal, sqrt, sin, asin, tan, cos), verified BIT-EXACT for all 1024
 * entries against a real dump -- so it is SYNTHESIZED and NO FIRMWARE FILE IS
 * REQUIRED, by developers or users.
 *
 * cx4_load_firmware() synthesizes, then looks for a dump
 * ($SNESRECOMP_CX4_ROM, ./cx4.rom, ./cx4.data.rom, ./firmware/cx4.rom, or the
 * game ROM's directory) purely to CROSS-CHECK the synthesis. A mismatch is
 * reported loudly and the file wins. Always returns 1: there is nothing to
 * fail at. */
int cx4_load_firmware(Cx4 *cx4, const char *rom_path);
/* Recompute the table from closed forms. Called by cx4_load_firmware; exposed
 * so a test or tool can build the table without any file. */
void cx4_synthesize_data_rom(Cx4 *cx4);
/* 1 once the table is populated (synthesis alone is sufficient). */
int cx4_firmware_loaded(const Cx4 *cx4);

/* ── observability ────────────────────────────────────────────────────────
 * Always-on ring of DSP program starts ($7F4F writes that un-halt the core),
 * recorded from device creation so probes read history rather than arming a
 * trace. `rdrom_hits` counts real RDROM executions, making data-ROM coverage
 * visible to tests and debug probes. */
typedef struct Cx4RunEvent {
  uint32_t seq;
  uint16_t pb;        /* entry program bank */
  uint8_t pc;         /* entry program counter (snes9x called this the "command") */
  uint32_t base;      /* cache base address in cartridge ROM */
} Cx4RunEvent;

enum { kCx4RunRingEntries = 4096 };

uint32_t cx4_run_ring_count(const Cx4 *cx4);
uint32_t cx4_run_ring_copy(const Cx4 *cx4, Cx4RunEvent *out, uint32_t max);
uint64_t cx4_instructions_executed(const Cx4 *cx4);
uint32_t cx4_rdrom_hits(const Cx4 *cx4);

/* Which data-ROM indices the game's Cx4 program actually reads.
 *
 * The 1024-entry table is four 256-entry sub-tables: [0..255] reciprocal
 * (floor(0x800000/n)), [256..511] square root (sqrt(n)*0x100000), and
 * [512..1023] a trigonometric pair. Knowing which blocks a title touches bounds
 * how much of the table has to be reproduced exactly — a block the games never
 * read cannot affect them.
 *
 * `block` is 0-3. cx4_rdrom_distinct returns how many of the 1024 entries have
 * been read at least once. */
uint32_t cx4_rdrom_block_hits(const Cx4 *cx4, unsigned block);
uint32_t cx4_rdrom_distinct(const Cx4 *cx4);
/* Lowest/highest index read so far, or 0/0 when nothing has been read. */
void cx4_rdrom_index_range(const Cx4 *cx4, uint32_t *lo, uint32_t *hi);
/* Set when the core wedged the bus (a DMA with both ends in the same space),
 * which on hardware requires a reset to clear. A loud stuck-state indicator. */
int cx4_locked(const Cx4 *cx4);

/* Clock extension for execution snapshots. The original RTLS Cx4 payload
 * predates this anchor; stream it from the versioned title extension. */
void cx4_saveload_clock(Cx4 *cx4, struct SaveLoadInfo *sli);

#endif /* SNES_CX4_H */
