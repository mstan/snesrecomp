/* Capcom Cx4 — instruction-level Hitachi HG51B S169 emulation.
 *
 * Ported from ares (ISC licence): ares/component/processor/hg51b/ plus
 * ares/sfc/coprocessor/hitachidsp/. See THIRD_PARTY_ATTRIBUTION.md.
 *
 * PORTING NOTE — READ BEFORE EDITING.
 * ares is written against nall's bit-precise integer types (n1/n5/n8/n15/n24/
 * n48), where every assignment silently truncates to the declared width. That
 * masking is load-bearing arithmetic, not decoration: the accumulator is 24
 * bits, the program counter is 8 bits and WRAPS to advance the cache page, the
 * multiplier is 48 bits, the page register is 15 bits. C has no such types, so
 * every width is enforced here by an explicit mask, and the masks are named
 * (M24, M15, ...) so a missing one is visible. A dropped mask does not fail to
 * compile and does not crash — it silently computes the wrong number, which is
 * the single most likely way for this file to be wrong.
 *
 * Structure mirrors the reference so the two can be diffed:
 *   registers        readRegister / writeRegister      (registers.cpp)
 *   algorithms       flag-setting ALU primitives       (instructions.cpp)
 *   instructions     one function per mnemonic         (instructions.cpp)
 *   decode           16-bit opcode -> instruction      (instruction.cpp)
 *   core             main/execute/advance/cache/dma    (hg51b.cpp)
 *   bus              SNES-side address decode + IO     (hitachidsp/memory.cpp)
 *
 * ares builds a 65536-entry std::function dispatch table at construction time.
 * That is replaced here by a two-level switch on the opcode's top 6 bits (and
 * top 8 where the encoding needs it), which is the same mapping expressed
 * directly — see cx4_execute_opcode.
 */
#include "cx4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "saveload.h"

/* Explicit width masks — see the porting note above. */
#define M1  0x1u
#define M2  0x3u
#define M4  0xFu
#define M5  0x1Fu
#define M7  0x7Fu
#define M8  0xFFu
#define M10 0x3FFu
#define M12 0xFFFu
#define M15 0x7FFFu
#define M16 0xFFFFu
#define M23 0x7FFFFFu
#define M24 0xFFFFFFu
#define M48 0xFFFFFFFFFFFFull

/* SNES master clock vs Cx4 oscillator. */
enum { kSnesMasterHz = 21477272, kCx4Hz = 20000000 };

struct Cx4 {
  /* ── HG51B core state ─────────────────────────────────────────────── */
  uint16_t programRAM[2][256];   /* instruction cache, two 256-word pages */
  uint32_t dataROM[1024];        /* 24-bit internal constant ROM (firmware) */
  uint8_t dataRAM[3072];

  struct {
    uint16_t pb;    /* 15: program bank */
    uint8_t pc;     /*  8: program counter — WRAPS to flip the cache page */
    uint8_t n, z, c, v, i;   /* flags, 0/1 */
    uint32_t a;     /* 24: accumulator */
    uint16_t p;     /* 15: page register */
    uint64_t mul;   /* 48: multiplier result */
    uint32_t mdr;   /* 24: bus memory data register */
    uint32_t rom;   /* 24: data ROM buffer */
    uint32_t ram;   /* 24: data RAM buffer */
    uint32_t mar;   /* 24: bus memory address register */
    uint32_t dpr;   /* 24: data RAM pointer */
    uint32_t gpr[16];  /* 24 each */
  } r;

  struct {
    uint8_t lock, halt, irq, rom;
    uint8_t vector[32];
    struct { uint8_t rom, ram; } wait;          /* 3 bits each */
    struct { uint8_t enable; uint8_t duration; } suspend;
    struct {
      uint8_t enable, page, lock[2];
      uint32_t address[2];   /* 24, in bytes */
      uint32_t base;         /* 24, in bytes */
      uint16_t pb;           /* 15 */
      uint8_t pc;
    } cache;
    struct { uint8_t enable; uint32_t source, target; uint16_t length; } dma;
    struct {
      uint8_t enable, reading, writing;
      uint8_t pending;       /* 4 bits */
      uint32_t address;      /* 24 */
    } bus;
  } io;

  uint32_t stack[8];   /* 23 bits each: pb<<8 | pc */

  /* ── host wiring ──────────────────────────────────────────────────── */
  const uint8_t *rom;
  uint32_t rom_size;
  uint8_t *ram;
  uint32_t ram_size;
  uint8_t mapping;        /* 0 for MMX2/X3 */
  uint8_t irq_line;       /* asserted CPU IRQ */
  int firmware_ok;

  uint64_t last_master;   /* master clock at the previous sync */
  int64_t budget;         /* Cx4 cycles available to run */
  uint32_t frac;          /* master->Cx4 rate-conversion remainder */
  int in_dsp;             /* re-entrancy guard: we are inside cx4_run */

  /* ── observability ────────────────────────────────────────────────── */
  Cx4RunEvent run_ring[kCx4RunRingEntries];
  uint32_t run_seq;
  uint64_t insns;
  uint32_t rdrom_hits;
  uint32_t rdrom_warned;
  /* Which data-ROM entries the guest's Cx4 program has actually read. A block
   * the games never touch cannot affect them, which bounds how much of the
   * table a synthesized replacement would have to reproduce exactly. */
  uint8_t rdrom_touched[1024 / 8];
  uint32_t rdrom_block_hits[4];
  uint32_t rdrom_lo, rdrom_hi;
  uint32_t rdrom_any;
};

/* Forward declarations for the SNES-side bus the core reads/writes through. */
static uint8_t cx4_bus_read(Cx4 *c, uint32_t addr24);
static void cx4_bus_write(Cx4 *c, uint32_t addr24, uint8_t data);
static int cx4_is_rom(const Cx4 *c, uint32_t addr24);
static int cx4_is_ram(const Cx4 *c, uint32_t addr24);
static void cx4_step(Cx4 *c, uint32_t clocks);
static void cx4_halt(Cx4 *c);

/* ── small helpers ────────────────────────────────────────────────────── */

/* Sign-extend the low `bits` of v, then mask back to 24 bits. */
static uint32_t sx24(uint32_t v, int bits) {
  const uint32_t sign = 1u << (bits - 1);
  v &= (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  if (v & sign) v |= ~((1u << bits) - 1u);
  return v & M24;
}

/* Arithmetic shift right of a 24-bit value, done explicitly rather than
 * relying on implementation-defined signed >>. */
static uint32_t asr24(uint32_t a, unsigned s) {
  const int neg = (a & 0x800000u) != 0;
  a &= M24;
  if (s >= 24) return neg ? M24 : 0;
  a >>= s;
  if (neg && s) a |= (M24 << (24 - s)) & M24;
  return a & M24;
}

static void cx4_set_byte(uint32_t *reg, unsigned byte, uint8_t v) {
  const unsigned sh = byte * 8u;
  *reg = (*reg & ~(0xFFu << sh)) | ((uint32_t)v << sh);
  *reg &= M24;
}

/* ── registers (registers.cpp) ────────────────────────────────────────── */

static uint32_t cx4_read_register(Cx4 *c, uint8_t address) {
  switch (address & M7) {
    case 0x01: return (uint32_t)((c->r.mul >> 24) & M24);
    case 0x02: return (uint32_t)(c->r.mul & M24);
    case 0x03: return c->r.mdr;
    case 0x08: return c->r.rom;
    case 0x0c: return c->r.ram;
    case 0x13: return c->r.mar;
    case 0x1c: return c->r.dpr;
    case 0x20: return c->r.pc;
    case 0x28: return c->r.p;
    /* Reading $2E/$2F starts a bus read into MDR; the value arrives later. */
    case 0x2e:
      c->io.bus.enable = 1;
      c->io.bus.reading = 1;
      c->io.bus.pending = (uint8_t)(1u + c->io.wait.rom);
      c->io.bus.address = c->r.mar;
      return 0x000000;
    case 0x2f:
      c->io.bus.enable = 1;
      c->io.bus.reading = 1;
      c->io.bus.pending = (uint8_t)(1u + c->io.wait.ram);
      c->io.bus.address = c->r.mar;
      return 0x000000;

    /* constant registers */
    case 0x50: return 0x000000;
    case 0x51: return 0xffffff;
    case 0x52: return 0x00ff00;
    case 0x53: return 0xff0000;
    case 0x54: return 0x00ffff;
    case 0x55: return 0xffff00;
    case 0x56: return 0x800000;
    case 0x57: return 0x7fffff;
    case 0x58: return 0x008000;
    case 0x59: return 0x007fff;
    case 0x5a: return 0xff7fff;
    case 0x5b: return 0xffff7f;
    case 0x5c: return 0x010000;
    case 0x5d: return 0xfeffff;
    case 0x5e: return 0x000100;
    case 0x5f: return 0x00feff;

    default:
      /* general purpose registers, mirrored at $60-$6F and $70-$7F */
      if ((address & 0x70) == 0x60 || (address & 0x70) == 0x70)
        return c->r.gpr[address & M4];
      return 0x000000;  /* verified open value on hardware */
  }
}

static void cx4_write_register(Cx4 *c, uint8_t address, uint32_t data) {
  data &= M24;
  switch (address & M7) {
    case 0x01: c->r.mul = ((c->r.mul & M24) | ((uint64_t)data << 24)) & M48; return;
    case 0x02: c->r.mul = ((c->r.mul & ~(uint64_t)M24) | data) & M48; return;
    case 0x03: c->r.mdr = data; return;
    case 0x08: c->r.rom = data; return;
    case 0x0c: c->r.ram = data; return;
    case 0x13: c->r.mar = data; return;
    case 0x1c: c->r.dpr = data; return;
    case 0x20: c->r.pc = (uint8_t)(data & M8); return;
    case 0x28: c->r.p = (uint16_t)(data & M15); return;
    case 0x2e:
      c->io.bus.enable = 1;
      c->io.bus.writing = 1;
      c->io.bus.pending = (uint8_t)(1u + c->io.wait.rom);
      c->io.bus.address = c->r.mar;
      return;
    case 0x2f:
      c->io.bus.enable = 1;
      c->io.bus.writing = 1;
      c->io.bus.pending = (uint8_t)(1u + c->io.wait.ram);
      c->io.bus.address = c->r.mar;
      return;
    default:
      if ((address & 0x70) == 0x60 || (address & 0x70) == 0x70)
        c->r.gpr[address & M4] = data;
      return;
  }
}

/* ── ALU primitives (instructions.cpp) ────────────────────────────────── */

static uint32_t alg_add(Cx4 *c, uint32_t x, uint32_t y) {
  x &= M24; y &= M24;
  const int32_t z = (int32_t)x + (int32_t)y;
  c->r.n = (z & 0x800000) != 0;
  c->r.z = ((uint32_t)z & M24) == 0;
  c->r.c = z > (int32_t)M24;
  c->r.v = (~(x ^ y) & (x ^ (uint32_t)z) & 0x800000u) != 0;
  return (uint32_t)z & M24;
}

static uint32_t alg_sub(Cx4 *c, uint32_t x, uint32_t y) {
  x &= M24; y &= M24;
  const int32_t z = (int32_t)x - (int32_t)y;
  c->r.n = (z & 0x800000) != 0;
  c->r.z = ((uint32_t)z & M24) == 0;
  c->r.c = z >= 0;
  /* The reference uses the ADD overflow form here too; kept verbatim so the
   * two implementations stay diffable rather than "corrected" on a hunch. */
  c->r.v = (~(x ^ y) & (x ^ (uint32_t)z) & 0x800000u) != 0;
  return (uint32_t)z & M24;
}

static uint32_t alg_nz(Cx4 *c, uint32_t x) {
  x &= M24;
  c->r.n = (x & 0x800000u) != 0;
  c->r.z = x == 0;
  return x;
}

static uint32_t alg_and(Cx4 *c, uint32_t x, uint32_t y) { return alg_nz(c, x & y); }
static uint32_t alg_or (Cx4 *c, uint32_t x, uint32_t y) { return alg_nz(c, x | y); }
static uint32_t alg_xor(Cx4 *c, uint32_t x, uint32_t y) { return alg_nz(c, x ^ y); }
static uint32_t alg_xnor(Cx4 *c, uint32_t x, uint32_t y) { return alg_nz(c, ~x ^ y); }
static uint32_t alg_sx(Cx4 *c, uint32_t x) { return alg_nz(c, x); }

static uint32_t alg_asr(Cx4 *c, uint32_t a, uint32_t s) {
  if (s > 24) s = 0;
  return alg_nz(c, asr24(a, s));
}

static uint32_t alg_shl(Cx4 *c, uint32_t a, uint32_t s) {
  if (s > 24) s = 0;
  return alg_nz(c, (a << s) & M24);
}

static uint32_t alg_shr(Cx4 *c, uint32_t a, uint32_t s) {
  if (s > 24) s = 0;
  return alg_nz(c, (a & M24) >> s);
}

static uint32_t alg_ror(Cx4 *c, uint32_t a, uint32_t s) {
  if (s > 24) s = 0;
  a &= M24;
  /* s == 0 and s == 24 both mean "no rotation"; writing it out avoids a
   * 24-bit shift whose overflow the reference only survives by truncation. */
  const uint32_t v = (s == 0 || s == 24)
      ? a
      : ((a >> s) | (a << (24 - s))) & M24;
  return alg_nz(c, v);
}

static uint64_t alg_mul(uint32_t x, uint32_t y) {
  /* Both operands are signed 24-bit; the product is 48 bits. */
  int64_t a = (int64_t)(x & M24);
  int64_t b = (int64_t)(y & M24);
  if (a & 0x800000) a -= 0x1000000;
  if (b & 0x800000) b -= 0x1000000;
  return (uint64_t)(a * b) & M48;
}

/* ── stack ────────────────────────────────────────────────────────────── */

static void cx4_push(Cx4 *c) {
  for (int i = 7; i > 0; i--) c->stack[i] = c->stack[i - 1];
  c->stack[0] = (((uint32_t)c->r.pb << 8) | c->r.pc) & M23;
}

static void cx4_pull(Cx4 *c) {
  const uint32_t pc = c->stack[0];
  for (int i = 0; i < 7; i++) c->stack[i] = c->stack[i + 1];
  c->stack[7] = 0;
  c->r.pb = (uint16_t)((pc >> 8) & M15);
  c->r.pc = (uint8_t)(pc & M8);
}

/* ── core control (hg51b.cpp) ─────────────────────────────────────────── */

static uint32_t cx4_wait(const Cx4 *c, uint32_t addr24) {
  if (cx4_is_rom(c, addr24)) return 1u + c->io.wait.rom;
  if (cx4_is_ram(c, addr24)) return 1u + c->io.wait.ram;
  return 1;
}

static void cx4_lock(Cx4 *c) { c->io.lock = 1; }

static void cx4_halt(Cx4 *c) {
  c->io.halt = 1;
  /* io.irq == 0 means IRQs are ENABLED (the bit is an inhibit). */
  if (c->io.irq == 0) {
    c->r.i = 1;
    c->irq_line = 1;
  }
}

static void cx4_step(Cx4 *c, uint32_t clocks) {
  c->budget -= (int64_t)clocks;
  if (!c->io.bus.enable) return;
  if (c->io.bus.pending > clocks) {
    c->io.bus.pending = (uint8_t)(c->io.bus.pending - clocks);
    return;
  }
  c->io.bus.enable = 0;
  c->io.bus.pending = 0;
  if (c->io.bus.reading) {
    c->io.bus.reading = 0;
    c->r.mdr = cx4_bus_read(c, c->io.bus.address);
  }
  if (c->io.bus.writing) {
    c->io.bus.writing = 0;
    cx4_bus_write(c, c->io.bus.address, (uint8_t)(c->r.mdr & M8));
  }
}

static int cx4_cache(Cx4 *c) {
  const uint32_t address = (c->io.cache.base + (uint32_t)c->r.pb * 512u) & M24;

  /* try the current page, then the other, then load into whichever is free */
  if (c->io.cache.address[c->io.cache.page] == address) {
    c->io.cache.enable = 0;
    return 1;
  }
  c->io.cache.page ^= 1;
  if (c->io.cache.address[c->io.cache.page] == address) {
    c->io.cache.enable = 0;
    return 1;
  }
  if (c->io.cache.lock[c->io.cache.page]) c->io.cache.page ^= 1;
  if (c->io.cache.lock[c->io.cache.page]) {
    c->io.cache.enable = 0;
    return 0;
  }

  c->io.cache.address[c->io.cache.page] = address;
  uint32_t a = address;
  for (unsigned offset = 0; offset < 256; offset++) {
    cx4_step(c, cx4_wait(c, a));
    const uint8_t lo = cx4_bus_read(c, a); a = (a + 1) & M24;
    const uint8_t hi = cx4_bus_read(c, a); a = (a + 1) & M24;
    c->programRAM[c->io.cache.page][offset] =
        (uint16_t)(lo | ((uint16_t)hi << 8));
  }
  c->io.cache.enable = 0;
  return 1;
}

static void cx4_advance(Cx4 *c) {
  c->r.pc = (uint8_t)((c->r.pc + 1) & M8);
  if (c->r.pc != 0) return;
  /* The 8-bit PC wrapping is how the core walks onto the second cache page. */
  if (c->io.cache.page == 1) { cx4_halt(c); return; }
  c->io.cache.page = 1;
  if (c->io.cache.lock[c->io.cache.page]) { cx4_halt(c); return; }
  c->r.pb = c->r.p;
  if (!cx4_cache(c)) cx4_halt(c);
}

static void cx4_suspend(Cx4 *c) {
  if (!c->io.suspend.duration) { cx4_step(c, 1); return; }  /* indefinite */
  cx4_step(c, c->io.suspend.duration);
  c->io.suspend.duration = 0;
  c->io.suspend.enable = 0;
}

static void cx4_dma(Cx4 *c) {
  for (uint32_t offset = 0; offset < c->io.dma.length; offset++) {
    const uint32_t source = (c->io.dma.source + offset) & M24;
    const uint32_t target = (c->io.dma.target + offset) & M24;
    /* Same-space transfers wedge the bus on hardware; reproduce that. */
    if (cx4_is_rom(c, source) && cx4_is_rom(c, target)) { cx4_lock(c); return; }
    if (cx4_is_ram(c, source) && cx4_is_ram(c, target)) { cx4_lock(c); return; }
    cx4_step(c, cx4_wait(c, source));
    const uint8_t data = cx4_bus_read(c, source);
    cx4_step(c, cx4_wait(c, target));
    cx4_bus_write(c, target, data);
  }
  c->io.dma.enable = 0;
}

static int cx4_running(const Cx4 *c) {
  return c->io.cache.enable || c->io.dma.enable || c->io.bus.pending ||
         !c->io.halt;
}

static int cx4_busy(const Cx4 *c) {
  return c->io.cache.enable || c->io.dma.enable || c->io.bus.pending;
}

static void cx4_execute_opcode(Cx4 *c, uint16_t opcode);

static void cx4_execute(Cx4 *c) {
  if (!cx4_cache(c)) { cx4_halt(c); return; }
  const uint16_t opcode = c->programRAM[c->io.cache.page][c->r.pc];
  cx4_advance(c);
  cx4_step(c, 1);
  c->insns++;
  cx4_execute_opcode(c, opcode);
}

static void cx4_main(Cx4 *c) {
  if (c->io.lock) { cx4_step(c, 1); return; }
  if (c->io.suspend.enable) { cx4_suspend(c); return; }
  if (c->io.cache.enable) { (void)cx4_cache(c); return; }
  if (c->io.dma.enable) { cx4_dma(c); return; }
  if (c->io.halt) { cx4_step(c, 1); return; }
  cx4_execute(c);
}

/* ── instruction semantics (instructions.cpp) ─────────────────────────── */

/* `A << shift` is computed at 24 bits because the reference passes it through
 * an n24 parameter, which truncates. */
static uint32_t acc_shifted(const Cx4 *c, unsigned shift) {
  return (c->r.a << shift) & M24;
}

static void insn_jmp(Cx4 *c, uint8_t data, int far, int take) {
  if (!take) return;
  if (far) c->r.pb = c->r.p;
  c->r.pc = data;
  cx4_step(c, 2);
}

static void insn_jsr(Cx4 *c, uint8_t data, int far, int take) {
  if (!take) return;
  cx4_push(c);
  if (far) c->r.pb = c->r.p;
  c->r.pc = data;
  cx4_step(c, 2);
}

static void insn_skip(Cx4 *c, int take, int flag) {
  if (flag != take) return;
  cx4_advance(c);
  cx4_step(c, 1);
}

static void insn_rdram(Cx4 *c, unsigned byte, uint32_t addr) {
  uint32_t address = addr & M12;
  if (address >= 0xc00) address -= 0x400;   /* $6C00-$6FFF mirrors down */
  cx4_set_byte(&c->r.ram, byte, c->dataRAM[address]);
}

static void insn_wrram(Cx4 *c, unsigned byte, uint32_t addr) {
  uint32_t address = addr & M12;
  if (address >= 0xc00) address -= 0x400;
  c->dataRAM[address] = (uint8_t)((c->r.ram >> (byte * 8u)) & M8);
}

static void insn_rdrom(Cx4 *c, uint32_t index) {
  const uint32_t idx = index & M10;
  c->rdrom_hits++;
  c->rdrom_touched[idx >> 3] |= (uint8_t)(1u << (idx & 7));
  c->rdrom_block_hits[idx >> 8]++;
  if (!c->rdrom_any) { c->rdrom_lo = c->rdrom_hi = idx; c->rdrom_any = 1; }
  else { if (idx < c->rdrom_lo) c->rdrom_lo = idx;
         if (idx > c->rdrom_hi) c->rdrom_hi = idx; }
  if (!c->firmware_ok && c->rdrom_warned < 1) {
    c->rdrom_warned++;
    fprintf(stderr,
        "[cx4] RDROM executed before the HG51B S169 data ROM was initialized —\n"
        "[cx4] every result derived from it will be wrong (reading zeros).\n"
        "[cx4] Call cx4_load_firmware() during cartridge initialization.\n");
  }
  c->r.rom = c->dataROM[index & M10] & M24;
}

static void cx4_execute_opcode(Cx4 *c, uint16_t opcode) {
  /* shift amounts selected by the 2-bit `ss` field of shifted-ALU opcodes */
  static const uint8_t shifts[4] = {0, 1, 8, 16};

  const unsigned group = (unsigned)(opcode >> 10) & 0x3F;  /* top 6 bits */
  const unsigned sub   = (unsigned)(opcode >> 8) & 0xFF;   /* top 8 bits */
  const unsigned reg   = opcode & M7;
  const unsigned imm8  = opcode & M8;
  const unsigned imm5  = opcode & M5;
  const unsigned sh    = shifts[(opcode >> 8) & M2];
  const int far        = (opcode >> 9) & M1;

  switch (group) {
    case 0x00: case 0x01: return;                       /* NOP */

    case 0x02: insn_jmp(c, (uint8_t)imm8, far, 1); return;
    case 0x03: insn_jmp(c, (uint8_t)imm8, far, c->r.z); return;   /* EQ */
    case 0x04: insn_jmp(c, (uint8_t)imm8, far, c->r.c); return;   /* GE */
    case 0x05: insn_jmp(c, (uint8_t)imm8, far, c->r.n); return;   /* MI */
    case 0x06: insn_jmp(c, (uint8_t)imm8, far, c->r.v); return;   /* VS */

    case 0x07:                                          /* WAIT */
      if (!c->io.bus.enable) return;
      cx4_step(c, c->io.bus.pending);
      return;

    case 0x08: return;                                  /* NOP */

    case 0x09:                                          /* SKIP <flag> */
      switch (sub) {
        case 0x24: insn_skip(c, opcode & M1, c->r.v); return;
        case 0x25: insn_skip(c, opcode & M1, c->r.c); return;
        case 0x26: insn_skip(c, opcode & M1, c->r.z); return;
        case 0x27: insn_skip(c, opcode & M1, c->r.n); return;
        default: return;
      }

    case 0x0A: insn_jsr(c, (uint8_t)imm8, far, 1); return;
    case 0x0B: insn_jsr(c, (uint8_t)imm8, far, c->r.z); return;
    case 0x0C: insn_jsr(c, (uint8_t)imm8, far, c->r.c); return;
    case 0x0D: insn_jsr(c, (uint8_t)imm8, far, c->r.n); return;
    case 0x0E: insn_jsr(c, (uint8_t)imm8, far, c->r.v); return;

    case 0x0F: cx4_pull(c); cx4_step(c, 2); return;     /* RTS */

    case 0x10: c->r.mar = (c->r.mar + 1) & M24; return; /* INC MAR */
    case 0x11: return;                                  /* NOP */

    case 0x12: alg_sub(c, cx4_read_register(c, (uint8_t)reg),
                       acc_shifted(c, sh)); return;     /* CMPR A<<s,reg */
    case 0x13: alg_sub(c, (uint32_t)imm8, acc_shifted(c, sh)); return;
    case 0x14: alg_sub(c, acc_shifted(c, sh),
                       cx4_read_register(c, (uint8_t)reg)); return;  /* CMP */
    case 0x15: alg_sub(c, acc_shifted(c, sh), (uint32_t)imm8); return;

    case 0x16:
      if (sub == 0x59) { c->r.a = alg_sx(c, sx24(c->r.a, 8)); return; }  /* SXB */
      if (sub == 0x5A) { c->r.a = alg_sx(c, sx24(c->r.a, 16)); return; } /* SXW */
      return;                                           /* NOP */
    case 0x17: return;                                  /* NOP */

    case 0x18:                                          /* LD <dst>,reg */
      switch (sub) {
        case 0x60: c->r.a   = cx4_read_register(c, (uint8_t)reg); return;
        case 0x61: c->r.mdr = cx4_read_register(c, (uint8_t)reg); return;
        case 0x62: c->r.mar = cx4_read_register(c, (uint8_t)reg); return;
        /* LD P,Rn takes a 4-bit GPR index and truncates to the 15-bit P. */
        case 0x63: c->r.p = (uint16_t)(c->r.gpr[opcode & M4] & M15); return;
        default: return;
      }

    case 0x19:                                          /* LD <dst>,imm */
      switch (sub) {
        case 0x64: c->r.a   = (uint32_t)imm8; return;
        case 0x65: c->r.mdr = (uint32_t)imm8; return;
        case 0x66: c->r.mar = (uint32_t)imm8; return;
        case 0x67: c->r.p   = (uint16_t)imm8; return;
        default: return;
      }

    case 0x1A:                                          /* RDRAM n,A */
      if (sub <= 0x6A) { insn_rdram(c, sub - 0x68, c->r.a); return; }
      return;
    case 0x1B:                                          /* RDRAM n,imm */
      if (sub <= 0x6E) { insn_rdram(c, sub - 0x6C, c->r.dpr + imm8); return; }
      return;

    case 0x1C: insn_rdrom(c, c->r.a); return;           /* RDROM A */
    case 0x1D: insn_rdrom(c, opcode & M10); return;     /* RDROM imm */
    case 0x1E: return;                                  /* NOP */

    case 0x1F:
      if (sub == 0x7C) {                                /* LD PL,imm */
        c->r.p = (uint16_t)((c->r.p & ~0xFFu) | imm8) & M15;
        return;
      }
      if (sub == 0x7D) {                                /* LD PH,imm (7 bits) */
        c->r.p = (uint16_t)((c->r.p & 0xFFu) |
                            ((uint32_t)(opcode & M7) << 8)) & M15;
        return;
      }
      return;                                           /* NOP */

    case 0x20: c->r.a = alg_add(c, acc_shifted(c, sh),
                                cx4_read_register(c, (uint8_t)reg)); return;
    case 0x21: c->r.a = alg_add(c, acc_shifted(c, sh), (uint32_t)imm8); return;
    case 0x22: c->r.a = alg_sub(c, cx4_read_register(c, (uint8_t)reg),
                                acc_shifted(c, sh)); return;   /* SUBR */
    case 0x23: c->r.a = alg_sub(c, (uint32_t)imm8, acc_shifted(c, sh)); return;
    case 0x24: c->r.a = alg_sub(c, acc_shifted(c, sh),
                                cx4_read_register(c, (uint8_t)reg)); return;
    case 0x25: c->r.a = alg_sub(c, acc_shifted(c, sh), (uint32_t)imm8); return;

    case 0x26: c->r.mul = alg_mul(c->r.a,
                                  cx4_read_register(c, (uint8_t)reg)); return;
    case 0x27: c->r.mul = alg_mul(c->r.a, (uint32_t)imm8); return;

    case 0x28: c->r.a = alg_xnor(c, acc_shifted(c, sh),
                                 cx4_read_register(c, (uint8_t)reg)); return;
    case 0x29: c->r.a = alg_xnor(c, acc_shifted(c, sh), (uint32_t)imm8); return;
    case 0x2A: c->r.a = alg_xor(c, acc_shifted(c, sh),
                                cx4_read_register(c, (uint8_t)reg)); return;
    case 0x2B: c->r.a = alg_xor(c, acc_shifted(c, sh), (uint32_t)imm8); return;
    case 0x2C: c->r.a = alg_and(c, acc_shifted(c, sh),
                                cx4_read_register(c, (uint8_t)reg)); return;
    case 0x2D: c->r.a = alg_and(c, acc_shifted(c, sh), (uint32_t)imm8); return;
    case 0x2E: c->r.a = alg_or(c, acc_shifted(c, sh),
                               cx4_read_register(c, (uint8_t)reg)); return;
    case 0x2F: c->r.a = alg_or(c, acc_shifted(c, sh), (uint32_t)imm8); return;

    /* The register-sourced shift count goes through an n5 parameter in the
     * reference, so only the low 5 bits select the distance. Passing the full
     * 24-bit register value here would make every count > 24 collapse to 0
     * (via the `s > 24` clamp) and silently produce a no-op shift. */
    case 0x30: c->r.a = alg_shr(c, c->r.a,
                   cx4_read_register(c, (uint8_t)reg) & M5); return;
    case 0x31: c->r.a = alg_shr(c, c->r.a, imm5); return;
    case 0x32: c->r.a = alg_asr(c, c->r.a,
                   cx4_read_register(c, (uint8_t)reg) & M5); return;
    case 0x33: c->r.a = alg_asr(c, c->r.a, imm5); return;
    case 0x34: c->r.a = alg_ror(c, c->r.a,
                   cx4_read_register(c, (uint8_t)reg) & M5); return;
    case 0x35: c->r.a = alg_ror(c, c->r.a, imm5); return;
    case 0x36: c->r.a = alg_shl(c, c->r.a,
                   cx4_read_register(c, (uint8_t)reg) & M5); return;
    case 0x37: c->r.a = alg_shl(c, c->r.a, imm5); return;

    case 0x38:                                          /* ST reg,<src> */
      if (sub == 0xE0) { cx4_write_register(c, (uint8_t)reg, c->r.a); return; }
      if (sub == 0xE1) { cx4_write_register(c, (uint8_t)reg, c->r.mdr); return; }
      return;
    case 0x39: return;                                  /* NOP */

    case 0x3A:                                          /* WRRAM n,A */
      if (sub <= 0xEA) { insn_wrram(c, sub - 0xE8, c->r.a); return; }
      return;
    case 0x3B:                                          /* WRRAM n,imm */
      if (sub <= 0xEE) { insn_wrram(c, sub - 0xEC, c->r.dpr + imm8); return; }
      return;

    case 0x3C: {                                        /* SWAP A,Rn */
      const unsigned n = opcode & M4;
      const uint32_t t = c->r.a;
      c->r.a = c->r.gpr[n];
      c->r.gpr[n] = t;
      return;
    }
    case 0x3D: return;                                  /* NOP */
    case 0x3E:                                          /* CLEAR */
      c->r.a = 0; c->r.p = 0; c->r.ram = 0; c->r.dpr = 0;
      return;
    case 0x3F: cx4_halt(c); return;                     /* HALT */

    default: return;
  }
}

/* ── SNES-side address decode (hitachidsp/memory.cpp) ─────────────────── */

/* Returns 1 and sets *out to a linear cartridge offset, or 0. */
static int addr_rom(const Cx4 *c, uint32_t address, uint32_t *out) {
  if ((address & 0x408000u) == 0x008000u ||
      (address & 0xc00000u) == 0xc00000u) {
    if (c->mapping == 0) {
      const uint32_t a = ((address & 0x3f0000u) >> 1) | (address & 0x7fffu);
      *out = a & 0x1fffffu;
    } else {
      *out = address & 0x3fffffu;
    }
    return 1;
  }
  return 0;
}

static int addr_ram(const Cx4 *c, uint32_t address, uint32_t *out) {
  if (c->mapping == 0) {
    if ((address & 0xf88000u) == 0x700000u) {   /* $70-$77:$0000-$7FFF */
      const uint32_t a = ((address & 0x070000u) >> 1) | (address & 0x7fffu);
      *out = a & 0x03ffffu;
      return 1;
    }
  } else {
    if ((address & 0x70e000u) == 0x306000u) {
      const uint32_t a = ((address & 0x0f0000u) >> 3) | (address & 0x1fffu);
      *out = a & 0x01ffffu;
      return 1;
    }
  }
  return 0;
}

/* 3 KB DSP data RAM: $6000-$6BFF and $7000-$7BFF (the $xC00-$xFFF hole is IO). */
static int addr_dram(const Cx4 *c, uint32_t address, uint32_t *out) {
  if ((address & 0x40e000u) == 0x006000u && (address & 0x0c00u) != 0x0c00u) {
    if (c->mapping != 0 && (address & 0x300000u) == 0x300000u) return 0;
    *out = address & 0x0fffu;
    return 1;
  }
  return 0;
}

/* IO window: $6C00-$6FFF and $7C00-$7FFF. */
static int addr_io(const Cx4 *c, uint32_t address, uint32_t *out) {
  if ((address & 0x40ec00u) == 0x006c00u) {
    if (c->mapping != 0 && (address & 0x300000u) == 0x300000u) return 0;
    *out = address & 0x03ffu;
    return 1;
  }
  return 0;
}

static int cx4_is_rom(const Cx4 *c, uint32_t addr24) {
  uint32_t t;
  return addr_rom(c, addr24, &t);
}

static int cx4_is_ram(const Cx4 *c, uint32_t addr24) {
  uint32_t t;
  return addr_ram(c, addr24, &t);
}

static uint8_t cx4_io_read(Cx4 *c, uint32_t address);
static void cx4_io_write(Cx4 *c, uint32_t address, uint8_t data);

static uint8_t cx4_dram_read(const Cx4 *c, uint32_t address) {
  address &= 0xfff;
  if (address >= 0xc00) return 0;
  return c->dataRAM[address];
}

static void cx4_dram_write(Cx4 *c, uint32_t address, uint8_t data) {
  address &= 0xfff;
  if (address >= 0xc00) return;
  c->dataRAM[address] = data;
}

/* The bus the DSP itself reads/writes through. */
static uint8_t cx4_bus_read(Cx4 *c, uint32_t addr24) {
  uint32_t lin;
  if (addr_rom(c, addr24, &lin))
    return (c->rom && c->rom_size) ? c->rom[lin % c->rom_size] : 0;
  if (addr_ram(c, addr24, &lin))
    return (c->ram && c->ram_size) ? c->ram[lin % c->ram_size] : 0;
  if (addr_dram(c, addr24, &lin)) return cx4_dram_read(c, lin);
  if (addr_io(c, addr24, &lin)) return cx4_io_read(c, lin);
  return 0;
}

static void cx4_bus_write(Cx4 *c, uint32_t addr24, uint8_t data) {
  uint32_t lin;
  if (addr_rom(c, addr24, &lin)) return;              /* ROM: writes ignored */
  if (addr_ram(c, addr24, &lin)) {
    if (c->ram && c->ram_size) c->ram[lin % c->ram_size] = data;
    return;
  }
  if (addr_dram(c, addr24, &lin)) { cx4_dram_write(c, lin, data); return; }
  if (addr_io(c, addr24, &lin)) { cx4_io_write(c, lin, data); return; }
}

/* ── IO registers ─────────────────────────────────────────────────────── */

static uint8_t cx4_io_read(Cx4 *c, uint32_t address) {
  address = 0x7c00u | (address & 0x03ffu);

  switch (address) {
    case 0x7f40: return (uint8_t)(c->io.dma.source & M8);
    case 0x7f41: return (uint8_t)((c->io.dma.source >> 8) & M8);
    case 0x7f42: return (uint8_t)((c->io.dma.source >> 16) & M8);
    case 0x7f43: return (uint8_t)(c->io.dma.length & M8);
    case 0x7f44: return (uint8_t)((c->io.dma.length >> 8) & M8);
    case 0x7f45: return (uint8_t)(c->io.dma.target & M8);
    case 0x7f46: return (uint8_t)((c->io.dma.target >> 8) & M8);
    case 0x7f47: return (uint8_t)((c->io.dma.target >> 16) & M8);
    case 0x7f48: return c->io.cache.page;
    case 0x7f49: return (uint8_t)(c->io.cache.base & M8);
    case 0x7f4a: return (uint8_t)((c->io.cache.base >> 8) & M8);
    case 0x7f4b: return (uint8_t)((c->io.cache.base >> 16) & M8);
    case 0x7f4c: return (uint8_t)(c->io.cache.lock[0] | (c->io.cache.lock[1] << 1));
    case 0x7f4d: return (uint8_t)(c->io.cache.pb & M8);
    case 0x7f4e: return (uint8_t)((c->io.cache.pb >> 8) & M8);
    case 0x7f4f: return c->io.cache.pc;
    case 0x7f50: return (uint8_t)(c->io.wait.ram | (c->io.wait.rom << 4));
    case 0x7f51: return c->io.irq;
    case 0x7f52: return c->io.rom;

    /* Status. THIS is what MMX2/X3 spin on: bit 6 = running, bit 7 = busy. */
    case 0x7f53: case 0x7f54: case 0x7f55: case 0x7f56: case 0x7f57:
    case 0x7f59: case 0x7f5b: case 0x7f5c: case 0x7f5d: case 0x7f5e:
    case 0x7f5f:
      return (uint8_t)(c->io.suspend.enable |
                       ((uint8_t)(c->r.i & 1) << 1) |
                       ((uint8_t)(cx4_running(c) ? 1 : 0) << 6) |
                       ((uint8_t)(cx4_busy(c) ? 1 : 0) << 7));
    default: break;
  }

  if (address >= 0x7f60 && address <= 0x7f7f)
    return c->io.vector[address & 0x1f];

  /* R0-R15, three bytes each, in two mirrored windows. */
  if ((address >= 0x7f80 && address <= 0x7faf) ||
      (address >= 0x7fc0 && address <= 0x7fef)) {
    const uint32_t a = address & 0x3f;
    return (uint8_t)((c->r.gpr[a / 3] >> ((a % 3) * 8u)) & M8);
  }
  return 0x00;
}

static void cx4_io_write(Cx4 *c, uint32_t address, uint8_t data) {
  address = 0x7c00u | (address & 0x03ffu);

  switch (address) {
    case 0x7f40: cx4_set_byte(&c->io.dma.source, 0, data); return;
    case 0x7f41: cx4_set_byte(&c->io.dma.source, 1, data); return;
    case 0x7f42: cx4_set_byte(&c->io.dma.source, 2, data); return;
    case 0x7f43: c->io.dma.length = (uint16_t)((c->io.dma.length & 0xFF00u) | data); return;
    case 0x7f44: c->io.dma.length = (uint16_t)((c->io.dma.length & 0x00FFu) | ((uint16_t)data << 8)); return;
    case 0x7f45: cx4_set_byte(&c->io.dma.target, 0, data); return;
    case 0x7f46: cx4_set_byte(&c->io.dma.target, 1, data); return;
    case 0x7f47:
      cx4_set_byte(&c->io.dma.target, 2, data);
      if (c->io.halt) c->io.dma.enable = 1;   /* writing byte 2 starts the DMA */
      return;

    case 0x7f48:
      c->io.cache.page = data & M1;
      if (c->io.halt) c->io.cache.enable = 1;
      return;

    case 0x7f49: cx4_set_byte(&c->io.cache.base, 0, data); return;
    case 0x7f4a: cx4_set_byte(&c->io.cache.base, 1, data); return;
    case 0x7f4b: cx4_set_byte(&c->io.cache.base, 2, data); return;

    case 0x7f4c:
      c->io.cache.lock[0] = data & M1;
      c->io.cache.lock[1] = (data >> 1) & M1;
      return;

    case 0x7f4d: c->io.cache.pb = (uint16_t)((c->io.cache.pb & 0xFF00u) | data) & M15; return;
    case 0x7f4e: c->io.cache.pb = (uint16_t)((c->io.cache.pb & 0x00FFu) | ((uint16_t)data << 8)) & M15; return;

    case 0x7f4f:
      /* Not a "command register": this is the entry program counter, and
       * writing it STARTS the DSP if it is halted. snes9x's command-level
       * model treated these values as opcodes; they are really addresses. */
      c->io.cache.pc = data;
      if (c->io.halt) {
        c->io.halt = 0;
        c->r.pb = c->io.cache.pb;
        c->r.pc = c->io.cache.pc;
        Cx4RunEvent *e = &c->run_ring[c->run_seq % kCx4RunRingEntries];
        e->seq = c->run_seq;
        e->pb = c->r.pb;
        e->pc = c->r.pc;
        e->base = c->io.cache.base;
        c->run_seq++;
      }
      return;

    case 0x7f50:
      c->io.wait.ram = data & 0x7u;
      c->io.wait.rom = (data >> 4) & 0x7u;
      return;

    case 0x7f51:
      c->io.irq = data & M1;
      if (c->io.irq == 1) { c->r.i = 0; c->irq_line = 0; }
      return;

    case 0x7f52: c->io.rom = data & M1; return;

    case 0x7f53: c->io.lock = 0; c->io.halt = 1; return;

    case 0x7f55: c->io.suspend.enable = 1; c->io.suspend.duration =   0; return;
    case 0x7f56: c->io.suspend.enable = 1; c->io.suspend.duration =  32; return;
    case 0x7f57: c->io.suspend.enable = 1; c->io.suspend.duration =  64; return;
    case 0x7f58: c->io.suspend.enable = 1; c->io.suspend.duration =  96; return;
    case 0x7f59: c->io.suspend.enable = 1; c->io.suspend.duration = 128; return;
    case 0x7f5a: c->io.suspend.enable = 1; c->io.suspend.duration = 160; return;
    case 0x7f5b: c->io.suspend.enable = 1; c->io.suspend.duration = 192; return;
    case 0x7f5c: c->io.suspend.enable = 1; c->io.suspend.duration = 224; return;
    case 0x7f5d: c->io.suspend.enable = 0; return;   /* resume */

    case 0x7f5e: c->r.i = 0; return;   /* does NOT deassert the CPU IRQ line */
    default: break;
  }

  if (address >= 0x7f60 && address <= 0x7f7f) {
    c->io.vector[address & 0x1f] = data;
    return;
  }

  if ((address >= 0x7f80 && address <= 0x7faf) ||
      (address >= 0x7fc0 && address <= 0x7fef)) {
    const uint32_t a = address & 0x3f;
    cx4_set_byte(&c->r.gpr[a / 3], a % 3, data);
    return;
  }
}

/* ── public interface ─────────────────────────────────────────────────── */

Cx4 *cx4_create(const uint8_t *rom, uint32_t rom_size,
                uint8_t *ram, uint32_t ram_size) {
  Cx4 *c = (Cx4 *)calloc(1, sizeof(Cx4));
  if (!c) return NULL;
  c->rom = rom;
  c->rom_size = rom_size;
  c->ram = ram;
  c->ram_size = ram_size;
  c->mapping = 0;      /* MMX2 / MMX3 */
  cx4_reset(c);
  return c;
}

void cx4_destroy(Cx4 *c) { free(c); }

void cx4_reset(Cx4 *c) {
  if (!c) return;
  memset(&c->r, 0, sizeof(c->r));
  memset(&c->io, 0, sizeof(c->io));
  memset(c->stack, 0, sizeof(c->stack));
  memset(c->programRAM, 0, sizeof(c->programRAM));
  memset(c->dataRAM, 0, sizeof(c->dataRAM));
  /* Power-on defaults from the reference's field initialisers. */
  c->io.halt = 1;
  c->io.rom = 1;
  c->io.wait.rom = 3;
  c->io.wait.ram = 3;
  c->irq_line = 0;
  c->budget = 0;
  c->frac = 0;
  c->last_master = 0;
  /* dataROM and the observability rings deliberately survive reset: the data
   * ROM is not guest state, and a probe after a soft reset still wants the
   * boot-time run history. */
}

void cx4_sync(Cx4 *c, uint64_t master_clock) {
  if (!c || c->in_dsp) return;      /* never re-enter from our own bus access */
  if (master_clock <= c->last_master) { c->last_master = master_clock; return; }

  /* Convert elapsed SNES master cycles into Cx4 cycles, carrying the
   * remainder so the 20 MHz / 21.477 MHz ratio does not drift. */
  const uint64_t delta = master_clock - c->last_master;
  c->last_master = master_clock;
  const uint64_t num = delta * (uint64_t)kCx4Hz + c->frac;
  c->budget += (int64_t)(num / (uint64_t)kSnesMasterHz);
  c->frac = (uint32_t)(num % (uint64_t)kSnesMasterHz);

  if (c->budget <= 0) return;

  /* Idle fast path: halted with nothing pending means every cx4_main() would
   * be a pure no-op step(1). Exactly equivalent, minus millions of calls. */
  if (!cx4_running(c) && !c->io.lock && !c->io.suspend.enable) {
    c->budget = 0;
    return;
  }

  c->in_dsp = 1;
  /* Every cx4_main() path either consumes at least one cycle or clears the
   * flag that selected it, so the budget loop terminates on its own. The guard
   * is a backstop against a future edit breaking that invariant, sized just
   * above the budget rather than arbitrarily large so a real hang is caught in
   * milliseconds instead of seconds. */
  int64_t guard = c->budget + 4096;
  while (c->budget > 0 && guard-- > 0) cx4_main(c);
  if (guard <= 0) {
    static int reported;
    if (!reported++)
      fprintf(stderr, "[cx4] sync guard tripped — DSP is not converging\n");
    c->budget = 0;
  }
  c->in_dsp = 0;
}

uint8_t cx4_read(Cx4 *c, uint16_t addr) {
  if (!c) return 0;
  const uint32_t a = 0x000000u | addr;   /* bank $00 is representative */
  uint32_t lin;
  if (addr_dram(c, a, &lin)) return cx4_dram_read(c, lin);
  if (addr_io(c, a, &lin)) return cx4_io_read(c, lin);
  return 0;
}

void cx4_write(Cx4 *c, uint16_t addr, uint8_t val) {
  if (!c) return;
  const uint32_t a = 0x000000u | addr;
  uint32_t lin;
  if (addr_dram(c, a, &lin)) { cx4_dram_write(c, lin, val); return; }
  if (addr_io(c, a, &lin)) { cx4_io_write(c, lin, val); return; }
}

uint8_t *cx4_ram_ptr(Cx4 *c, uint16_t addr) {
  if (!c) return NULL;
  uint32_t lin;
  if (!addr_dram(c, 0x000000u | addr, &lin)) return NULL;  /* IO -> NULL */
  lin &= 0xfff;
  if (lin >= 0xc00) return NULL;
  return &c->dataRAM[lin];
}

int cx4_owns_bus(const Cx4 *c) { return c && cx4_busy(c); }

uint8_t cx4_read_vector_override(Cx4 *c, uint16_t addr) {
  /* While the DSP holds the bus, CPU reads of the vector area return Cx4 IO
   * instead of ROM — that is how the coprocessor overrides the vectors. */
  return cx4_io_read(c, 0x7f40u | (addr & 0x3fu));
}

int cx4_irq_pending(const Cx4 *c) { return c ? c->irq_line : 0; }
void cx4_irq_acknowledge(Cx4 *c) { if (c) c->irq_line = 0; }

int cx4_firmware_loaded(const Cx4 *c) { return c ? c->firmware_ok : 0; }

/* ── data ROM synthesis ───────────────────────────────────────────────────
 *
 * The HG51B S169's 1024-entry x 24-bit internal data ROM is not a blob of
 * authored data — it is six closed-form mathematical tables, which means it can
 * be computed instead of supplied. Verified BIT-EXACT for all 1024 entries
 * against a real dump (see tests/test_cx4_datarom.py):
 *
 *   [  0.. 255]  floor(2^23 / n)                 reciprocal; n=0 saturates
 *   [256.. 511]  floor(sqrt(n) * 2^20)           square root
 *   [512.. 639]  floor(2^24 * sin(n*pi/256))     sine,     n=0..127
 *   [640.. 767]  floor((2^24/pi) * asin(n/128))  arcsine,  n=0..127
 *   [768.. 895]  floor(2^16 * tan(n*pi/256))     tangent,  n=0..127
 *   [896..1023]  floor(2^24 * cos(n*pi/256))     cosine,   n=0..127
 *
 * Precision is not luck. Only five entries land within 1e-9 of a floor
 * boundary, and all five are exact mathematical values: sin(0)=0, asin(0)=0,
 * tan(0)=0, cos(0)=1 (which needs 2^24 and therefore saturates to 0xFFFFFF in a
 * 24-bit field), and tan(45deg)=1. The nearest genuinely-irrational entry sits
 * 7.2e-4 from an integer — about 1e11 double-epsilons — so no libm variation can
 * tip a floor. The two unit values are handled explicitly below; everything else
 * is plain doubles.
 *
 * Consequence: cx4.rom is OPTIONAL. Nobody needs the firmware file. When one IS
 * present it is used as a verification oracle instead of a dependency.
 */
static uint64_t cx4_isqrt64(uint64_t v) {
  /* Exact integer square root; floor(sqrt(n) * 2^20) == isqrt(n << 40), which
   * avoids trusting a double for the sqrt block entirely. */
  if (v == 0) return 0;
  uint64_t x = v, y = (x + 1) / 2;
  while (y < x) { x = y; y = (x + v / x) / 2; }
  while ((x + 1) * (x + 1) <= v) x++;
  while (x * x > v) x--;
  return x;
}

void cx4_synthesize_data_rom(Cx4 *c) {
  /* Full double precision: this literal rounds to the same double as the
   * reference implementation's pi, which is what makes the match reproducible. */
  static const double kPi = 3.14159265358979323846;
  if (!c) return;

  for (unsigned n = 0; n < 256; n++)
    c->dataROM[n] = n ? (0x800000u / n) : 0xFFFFFFu;

  for (unsigned n = 0; n < 256; n++)
    c->dataROM[256 + n] = (uint32_t)cx4_isqrt64((uint64_t)n << 40) & M24;

  for (unsigned n = 0; n < 128; n++) {
    const double a = kPi * (double)n / 256.0;
    c->dataROM[512 + n] = (uint32_t)floor(16777216.0 * sin(a)) & M24;
    c->dataROM[640 + n] =
        (uint32_t)floor((16777216.0 / kPi) * asin((double)n / 128.0)) & M24;
    /* tan(45deg) is exactly 1, but libm's tan(pi/4) lands just below it and
     * floor() would then yield 0xFFFF instead of 0x10000. */
    c->dataROM[768 + n] = (n == 64)
        ? 0x010000u
        : (uint32_t)floor(65536.0 * tan(a)) & M24;
    /* cos(0) = 1 needs 2^24, which does not fit 24 bits: hardware saturates. */
    const double cv = floor(16777216.0 * cos(a));
    c->dataROM[896 + n] = (cv > 16777215.0) ? 0xFFFFFFu : (uint32_t)cv & M24;
  }
  c->firmware_ok = 1;
}

/* Load a firmware dump into a scratch buffer and diff it against whatever is
 * currently in dataROM. Used to turn a user-supplied cx4.rom into a SELF-CHECK
 * on the synthesizer rather than a dependency. */
static int cx4_verify_against_file(Cx4 *c, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  uint8_t buf[3072];
  const size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  if (n != sizeof(buf)) {
    fprintf(stderr, "[cx4] %s is %u bytes, expected 3072 — ignoring\n",
            path, (unsigned)n);
    return 0;
  }
  unsigned bad = 0, first = 0;
  for (unsigned i = 0; i < 1024; i++) {
    const uint32_t v = (uint32_t)buf[i * 3] |
                       ((uint32_t)buf[i * 3 + 1] << 8) |
                       ((uint32_t)buf[i * 3 + 2] << 16);
    if (v != c->dataROM[i]) { if (!bad) first = i; bad++; }
  }
  if (bad == 0) {
    fprintf(stderr,
            "[cx4] data ROM synthesis VERIFIED bit-exact against %s "
            "(1024/1024)\n", path);
    return 1;
  }
  fprintf(stderr,
      "\n[cx4] ***  SYNTHESIS MISMATCH  ***\n"
      "[cx4] %u of 1024 synthesized entries differ from %s (first at %u:\n"
      "[cx4] synth=%#08x file=%#08x). Trusting the FILE and continuing.\n"
      "[cx4] This means the closed-form model is wrong on this platform's\n"
      "[cx4] libm — please report it; see tests/test_cx4_datarom.py.\n\n",
      bad, path, first, c->dataROM[first],
      (uint32_t)buf[first * 3] | ((uint32_t)buf[first * 3 + 1] << 8) |
          ((uint32_t)buf[first * 3 + 2] << 16));
  /* The dump is ground truth; prefer it over our model when they disagree. */
  for (unsigned i = 0; i < 1024; i++)
    c->dataROM[i] = (uint32_t)buf[i * 3] |
                    ((uint32_t)buf[i * 3 + 1] << 8) |
                    ((uint32_t)buf[i * 3 + 2] << 16);
  return 1;
}

int cx4_load_firmware(Cx4 *c, const char *rom_path) {
  if (!c) return 0;

  /* Always synthesize first: the data ROM is six closed-form tables, verified
   * bit-exact for all 1024 entries, so no firmware file is required by anyone.
   * A dump, if the developer happens to have one, becomes a self-check. */
  cx4_synthesize_data_rom(c);

  char buf[1024];
  const char *env = getenv("SNESRECOMP_CX4_ROM");
  if (env && env[0] && cx4_verify_against_file(c, env)) return 1;
  if (cx4_verify_against_file(c, "cx4.rom")) return 1;
  if (cx4_verify_against_file(c, "cx4.data.rom")) return 1;
  if (cx4_verify_against_file(c, "firmware/cx4.rom")) return 1;
  if (rom_path && rom_path[0]) {
    size_t n = strlen(rom_path);
    if (n < sizeof(buf) - 16) {
      memcpy(buf, rom_path, n + 1);
      while (n && buf[n - 1] != '/' && buf[n - 1] != '\\') n--;
      buf[n] = '\0';
      strcat(buf, "cx4.rom");
      if (cx4_verify_against_file(c, buf)) return 1;
    }
  }
  /* No dump to cross-check against. Not a problem — just say so once, quietly,
   * so a developer who DOES have one knows it went unused. */
  fprintf(stderr,
          "[cx4] data ROM synthesized from closed forms (no cx4.rom present to "
          "cross-check against; none is required)\n");
  return 1;
}

void cx4_saveload_clock(Cx4 *c, struct SaveLoadInfo *sli) {
  if (c && sli) sli->func(sli, &c->last_master, sizeof(c->last_master));
}

void cx4_saveload(Cx4 *c, struct SaveLoadInfo *sli) {
  if (!c || !sli) return;
  /* Guest-visible device state only. The data ROM is static; the rings are host
   * observability and are deliberately not snapshotted. */
  sli->func(sli, c->programRAM, sizeof(c->programRAM));
  sli->func(sli, c->dataRAM, sizeof(c->dataRAM));
  sli->func(sli, &c->r, sizeof(c->r));
  sli->func(sli, &c->io, sizeof(c->io));
  sli->func(sli, c->stack, sizeof(c->stack));
  sli->func(sli, &c->irq_line, sizeof(c->irq_line));
  sli->func(sli, &c->budget, sizeof(c->budget));
  sli->func(sli, &c->frac, sizeof(c->frac));
}

uint32_t cx4_run_ring_count(const Cx4 *c) { return c ? c->run_seq : 0; }
uint64_t cx4_instructions_executed(const Cx4 *c) { return c ? c->insns : 0; }
uint32_t cx4_rdrom_hits(const Cx4 *c) { return c ? c->rdrom_hits : 0; }

uint32_t cx4_rdrom_block_hits(const Cx4 *c, unsigned block) {
  return (c && block < 4) ? c->rdrom_block_hits[block] : 0;
}

uint32_t cx4_rdrom_distinct(const Cx4 *c) {
  if (!c) return 0;
  uint32_t n = 0;
  for (unsigned i = 0; i < sizeof(c->rdrom_touched); i++) {
    uint8_t b = c->rdrom_touched[i];
    while (b) { n += (b & 1u); b = (uint8_t)(b >> 1); }
  }
  return n;
}

void cx4_rdrom_index_range(const Cx4 *c, uint32_t *lo, uint32_t *hi) {
  if (lo) *lo = (c && c->rdrom_any) ? c->rdrom_lo : 0;
  if (hi) *hi = (c && c->rdrom_any) ? c->rdrom_hi : 0;
}
int cx4_locked(const Cx4 *c) { return c ? c->io.lock : 0; }

uint32_t cx4_run_ring_copy(const Cx4 *c, Cx4RunEvent *out, uint32_t max) {
  if (!c || !out || max == 0) return 0;
  const uint32_t have =
      c->run_seq < kCx4RunRingEntries ? c->run_seq : kCx4RunRingEntries;
  const uint32_t n = have < max ? have : max;
  const uint32_t first = c->run_seq - n;
  for (uint32_t i = 0; i < n; i++)
    out[i] = c->run_ring[(first + i) % kCx4RunRingEntries];
  return n;
}
