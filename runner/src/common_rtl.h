#pragma once
#include "types.h"
#include "snes/snes_regs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct SimpleHdma {
  const uint8 *table;
  const uint8 *indir_ptr;
  uint8 rep_count;
  uint8 mode;
  uint8 ppu_addr;
  uint8 indir_bank;
} SimpleHdma;


typedef struct Dma Dma;
typedef struct DmaChannel DmaChannel;
typedef struct Ppu Ppu;

void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc);
void SimpleHdma_DoLine(SimpleHdma *c);

extern uint8 g_ram[0x20000];
extern uint8 *g_sram;
extern int g_sram_size;
extern const uint8 *g_rom;
extern Ppu *g_ppu;
extern Dma *g_dma;

#define GET_BYTE(p) (*(uint8*)(p))

extern int snes_frame_counter;

#include "spc_player.h"

void MemCpy(void *dst, const void *src, int size);
bool Unreachable();

#if defined(_DEBUG)
// Gives better warning messages but non inlined on tcc
static inline uint16 GET_WORD(const uint8 *p) { return *(uint16 *)(p); }
static inline const uint8 *RomFixedPtr(uint32_t addr) { return &g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff]; }
#else
#define GET_WORD(p) (*(uint16*)(p))
#define RomFixedPtr(addr) (&g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff])
#endif

#define GET_BYTE(p) (*(uint8*)(p))
#define SET_WORD(p, v) (*(uint16*)(p) = (uint16)(v))

// Construct a LongPtr from a 16-bit lo word and 8-bit bank byte.
// Used by the DP aliasing fix: local pointer variables replace g_ram reads.
static inline LongPtr MAKE_LONG(uint16 lo, uint8 bank) {
  LongPtr lp;
  *(uint16 *)&lp = lo;
  ((uint8 *)&lp)[2] = bank;
  return lp;
}

uint8 *RomPtr(uint32_t addr);
uint8 *MvnPtr(uint8_t bank, uint16_t addr);

static inline uint8 *RomPtr_RAM(uint16_t addr) { assert(addr < 0x2000); return g_ram + addr; }
static inline const uint8 *RomPtr_00(uint16_t addr) { return RomPtr(0x000000 | addr); }
static inline const uint8 *RomPtr_01(uint16_t addr) { return RomPtr(0x010000 | addr); }
static inline const uint8 *RomPtr_02(uint16_t addr) { return RomPtr(0x020000 | addr); }
static inline const uint8 *RomPtr_03(uint16_t addr) { return RomPtr(0x030000 | addr); }
static inline const uint8 *RomPtr_04(uint16_t addr) { return RomPtr(0x040000 | addr); }
static inline const uint8 *RomPtr_05(uint16_t addr) { return RomPtr(0x050000 | addr); }
static inline const uint8 *RomPtr_06(uint16_t addr) { return RomPtr(0x060000 | addr); }
static inline const uint8 *RomPtr_07(uint16_t addr) { return RomPtr(0x070000 | addr); }
static inline const uint8 *RomPtr_08(uint16_t addr) { return RomPtr(0x080000 | addr); }
static inline const uint8 *RomPtr_09(uint16_t addr) { return RomPtr(0x090000 | addr); }
static inline const uint8 *RomPtr_0A(uint16_t addr) { return RomPtr(0x0a0000 | addr); }
static inline const uint8 *RomPtr_0B(uint16_t addr) { return RomPtr(0x0b0000 | addr); }
static inline const uint8 *RomPtr_0C(uint16_t addr) { return RomPtr(0x0c0000 | addr); }
static inline const uint8 *RomPtr_0D(uint16_t addr) { return RomPtr(0x0d0000 | addr); }
static inline const uint8 *RomPtr_0E(uint16_t addr) { return RomPtr(0x0e0000 | addr); }
static inline const uint8 *RomPtr_0F(uint16_t addr) { return RomPtr(0x0f0000 | addr); }
static inline const uint8 *RomPtr_11(uint16_t addr) { return RomPtr(0x110000 | addr); }
static inline const uint8 *RomPtr_12(uint16_t addr) { return RomPtr(0x120000 | addr); }
// Extended ROM banks (used in data banks and bank mirrors)
static inline const uint8 *RomPtr_18(uint16_t addr) { return RomPtr(0x180000 | addr); }
static inline const uint8 *RomPtr_1D(uint16_t addr) { return RomPtr(0x1d0000 | addr); }
static inline const uint8 *RomPtr_20(uint16_t addr) { return RomPtr(0x200000 | addr); }
static inline const uint8 *RomPtr_28(uint16_t addr) { return RomPtr(0x280000 | addr); }
static inline const uint8 *RomPtr_37(uint16_t addr) { return RomPtr(0x370000 | addr); }
static inline const uint8 *RomPtr_38(uint16_t addr) { return RomPtr(0x380000 | addr); }
static inline const uint8 *RomPtr_39(uint16_t addr) { return RomPtr(0x390000 | addr); }
static inline const uint8 *RomPtr_40(uint16_t addr) { return RomPtr(0x400000 | addr); }
static inline const uint8 *RomPtr_42(uint16_t addr) { return RomPtr(0x420000 | addr); }
static inline const uint8 *RomPtr_44(uint16_t addr) { return RomPtr(0x440000 | addr); }
static inline const uint8 *RomPtr_48(uint16_t addr) { return RomPtr(0x480000 | addr); }
static inline const uint8 *RomPtr_4B(uint16_t addr) { return RomPtr(0x4b0000 | addr); }
static inline const uint8 *RomPtr_66(uint16_t addr) { return RomPtr(0x660000 | addr); }
static inline const uint8 *RomPtr_6B(uint16_t addr) { return RomPtr(0x6b0000 | addr); }
static inline const uint8 *RomPtr_6D(uint16_t addr) { return RomPtr(0x6d0000 | addr); }
static inline const uint8 *RomPtr_7B(uint16_t addr) { return RomPtr(0x7b0000 | addr); }
// High bank mirrors ($80+) and upper data banks
static inline const uint8 *RomPtr_82(uint16_t addr) { return RomPtr(0x820000 | addr); }
static inline const uint8 *RomPtr_87(uint16_t addr) { return RomPtr(0x870000 | addr); }
static inline const uint8 *RomPtr_89(uint16_t addr) { return RomPtr(0x890000 | addr); }
static inline const uint8 *RomPtr_8A(uint16_t addr) { return RomPtr(0x8a0000 | addr); }
static inline const uint8 *RomPtr_8C(uint16_t addr) { return RomPtr(0x8c0000 | addr); }
static inline const uint8 *RomPtr_90(uint16_t addr) { return RomPtr(0x900000 | addr); }
static inline const uint8 *RomPtr_94(uint16_t addr) { return RomPtr(0x940000 | addr); }
static inline const uint8 *RomPtr_A0(uint16_t addr) { return RomPtr(0xa00000 | addr); }
static inline const uint8 *RomPtr_A8(uint16_t addr) { return RomPtr(0xa80000 | addr); }
static inline const uint8 *RomPtr_AE(uint16_t addr) { return RomPtr(0xae0000 | addr); }
static inline const uint8 *RomPtr_B7(uint16_t addr) { return RomPtr(0xb70000 | addr); }
static inline const uint8 *RomPtr_C9(uint16_t addr) { return RomPtr(0xc90000 | addr); }
static inline const uint8 *RomPtr_D6(uint16_t addr) { return RomPtr(0xd60000 | addr); }
static inline const uint8 *RomPtr_F8(uint16_t addr) { return RomPtr(0xf80000 | addr); }
static inline const uint8 *RomPtrWithBank(uint8 bank, uint16_t addr) { return RomPtr((bank << 16) | addr); }
// WRAM banks — $7E:xxxx → g_ram[addr], $7F:xxxx → g_ram[0x10000 + addr]
static inline uint8 *RomPtr_7E(uint16_t addr) { return g_ram + addr; }
static inline uint8 *RomPtr_7F(uint16_t addr) { return g_ram + 0x10000 + addr; }
static inline const uint8 *RomPtr_10(uint16_t addr) { return RomPtr(0x100000 | addr); }
static inline const uint8 *RomPtr_17(uint16_t addr) { return RomPtr(0x170000 | addr); }
static inline const uint8 *RomPtr_1B(uint16_t addr) { return RomPtr(0x1b0000 | addr); }
static inline const uint8 *RomPtr_1C(uint16_t addr) { return RomPtr(0x1c0000 | addr); }
static inline const uint8 *RomPtr_80(uint16_t addr) { return RomPtr(0x000000 | addr); }
// RomPtr_88 is emitted only inside auto_01_8636 — a REVIEW-flagged
// function the recompiler auto-promoted from a garbled decode at
// $01:8636 ("garbled JSL operands; RomPtr with invalid banks (F8,88,B9)
// --data decoded as code"). Grep confirms auto_01_8636 has zero callers,
// so this stub is never executed at runtime; kept only so smw_01_gen.c
// still compiles. Real fix is a recomp.py pass that suppresses
// zero-caller REVIEW functions at generation time, at which point this
// stub (and the dead function body) both go.
static inline const uint8 *RomPtr_88(uint16_t addr) { (void)addr; return g_rom; }

void WriteReg(uint16 reg, uint8 value);
void WriteRegWord(uint16 reg, uint16 value);
uint16 ReadRegWord(uint16 reg);
uint8 ReadReg(uint16 reg);
uint8_t *IndirPtr_Slow(LongPtr ptr, uint16 offs);

// 16-bit-indirect-via-DP resolution. The addressing modes `(dp)`,
// `(dp),Y`, `(dp,X)` and `(dp,S),Y` all fetch a 2-byte pointer from
// DP and combine it with the data bank register (DB) to form the
// full 24-bit effective address. Use this instead of raw
// `g_ram[ptr_lo | ptr_hi<<8]` — that silently assumes DB=\$7E and
// returns garbage when DB is a ROM bank (typical for in-ROM
// data-table loads).
uint8_t *IndirPtrDB(uint8 dp_addr, uint16 offs);
static inline uint8_t *IndirPtr(LongPtr ptr, uint16 offs) {
  uint32 a = (*(uint32 *)&ptr & 0xffffff) + offs;
  uint8 bank = (uint8)(a >> 16);
  if (bank >= 0x7e && bank <= 0x7f)
    return &g_ram[a & 0x1ffff];
  if ((a & 0xffff) < 0x2000)
    return &g_ram[a & 0x1ffff];
  return RomPtr(a);
}
static inline void IndirWriteByte(LongPtr ptr, uint16 offs, uint8 value) {
  uint8_t *dst = IndirPtr(ptr, offs);
  dst[0] = value;
}

// 16-bit word store through a 24-bit DP pointer. Native counterpart of
// `STA [dp]` / `STA [dp],Y` emitted when M=0 (A-16). Writes the low byte
// at the effective address and the high byte one byte later; the pair is
// always contiguous in the target region (WRAM or ROM-mirror).
static inline void IndirWriteWord(LongPtr ptr, uint16 offs, uint16 value) {
  uint8_t *dst = IndirPtr(ptr, offs);
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
}

void RtlReset(int mode);

enum {
  kSaveLoad_Save = 1,
  kSaveLoad_Load = 2,
};

void RtlSaveLoad(int cmd, int slot);
void RtlApuLock();
void RtlApuUnlock();
void RtlRenderAudio(int16 *audio_buffer, int samples, int channels);
bool RtlRunFrame(uint32 inputs);
void RtlReadSram();
void RtlWriteSram();
void RtlSaveSnapshot(const char *filename);
bool RtlLoadSnapshot(const char *filename);

void RtlApuWrite(uint16 adr, uint8 val);


enum {
  kJoypadL_A = 0x80,
  kJoypadL_X = 0x40,
  kJoypadL_L = 0x20,
  kJoypadL_R = 0x10,

  kJoypadH_B = 0x80,
  kJoypadH_Y = 0x40,
  kJoypadH_Select = 0x20,
  kJoypadH_Start = 0x10,

  kJoypadH_Up = 0x8,
  kJoypadH_Down = 0x4,
  kJoypadH_Left = 0x2,
  kJoypadH_Right = 0x1,

  kJoypadH_AnyDir = 0xf,
};