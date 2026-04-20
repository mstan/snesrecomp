#pragma once
#include "types.h"
#include "snes/snes_regs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// PRINCIPLES.md rule 20: NO STUBS EVER.
// When the recompiler can't determine a register/parameter value at codegen
// time, it MUST emit one of these calls instead of silently substituting 0.
// At runtime any execution path through one will print the violating ROM
// address and abort, making the divergence observable instead of corrupting
// state silently. To eliminate a violation: add the appropriate cfg hint
// (ret_y, RetAY, restores_x, x_after, preserves_y, etc.) and regenerate.
void _rule20_die(uint32 addr, const char *kind);
// Single macro: kind is a string literal embedded at the call site.
// _rule20_die never returns (it calls abort()), so the trailing 0 is dead code,
// but the comma operator makes the macro usable as an expression that has
// the right type at the use site.
#define Rule20Violation(addr, kind) (_rule20_die((addr), (kind)), 0)

enum {
  kGameID_SMW = 1,
  kGameID_SMB1 = 2,
  kGameID_SMBLL = 3,
};

enum {
  // Version was bumped to 1 after I fixed bug #1
  kCurrentBugFixCounter = 1,
};

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
void RtlHdmaSetup(uint8 which, uint8 transfer_unit, uint8 reg, uint32 addr, uint8 indirect_bank);

extern uint8 g_ram[0x20000];
extern uint8 *g_sram;
extern int g_sram_size;
extern const uint8 *g_rom;
extern Ppu *g_ppu;
extern Dma *g_dma;

#define GET_BYTE(p) (*(uint8*)(p))

extern int snes_frame_counter;
extern bool g_debug_flag;
extern uint8 game_id;

typedef struct SpcPlayer SpcPlayer;
extern SpcPlayer *g_spc_player;

void mov24(LongPtr *dst, uint32 src);
uint32 Load24(LongPtr src);
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
// $FF, $AA, $93, $13, $2A, $A5, $62, $9F, $A9, $E2 etc. are dead-code / invalid banks — should never execute; stubs to avoid compile error
static inline uint8 *RomPtr_FF(uint16_t addr) { (void)addr; return g_ram; }
static inline const uint8 *RomPtr_AA(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_93(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_13(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_2A(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_A5(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_62(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_9B(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_9F(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_A9(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_E2(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_84(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_85(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_88(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_B9(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_10(uint16_t addr) { return RomPtr(0x100000 | addr); }
static inline const uint8 *RomPtr_17(uint16_t addr) { return RomPtr(0x170000 | addr); }
static inline const uint8 *RomPtr_1B(uint16_t addr) { return RomPtr(0x1b0000 | addr); }
static inline const uint8 *RomPtr_1C(uint16_t addr) { return RomPtr(0x1c0000 | addr); }
static inline const uint8 *RomPtr_71(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_80(uint16_t addr) { return RomPtr(0x000000 | addr); }
static inline const uint8 *RomPtr_E0(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_E5(uint16_t addr) { (void)addr; return g_rom; }
static inline const uint8 *RomPtr_F0(uint16_t addr) { (void)addr; return g_rom; }

void AddHiLo(uint8 *hi, uint8 *lo, uint16 v);
void SetHiLo(uint8 *hi, uint8 *lo, uint16 v);
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
void debug_server_log_map16_write(uint16_t ram_addr, uint8_t value,
                                   uint16_t ptr_lo, uint8_t ptr_bank,
                                   uint16_t offset);
static inline void IndirWriteByte(LongPtr ptr, uint16 offs, uint8 value) {
  uint8_t *dst = IndirPtr(ptr, offs);
  // Map16 write instrumentation: check if writing to WRAM $C800-$CFFF
  uint32 a = (*(uint32 *)&ptr & 0xffffff) + offs;
  uint16_t ram_addr = (uint16_t)(a & 0x1ffff);
  if (ram_addr >= 0xC800 && ram_addr <= 0xCFFF) {
    debug_server_log_map16_write(ram_addr, value,
      (uint16_t)(*(uint32 *)&ptr & 0xffff),
      (uint8_t)((*(uint32 *)&ptr >> 16) & 0xff),
      offs);
  }
  dst[0] = value;
}

// 16-bit word store through a 24-bit DP pointer. Native counterpart of
// `STA [dp]` / `STA [dp],Y` emitted when M=0 (A-16). Writes the low byte
// at the effective address and the high byte one byte later; the pair is
// always contiguous in the target region (WRAM or ROM-mirror). Mirrors
// IndirWriteByte's Map16 instrumentation for both bytes of the word.
static inline void IndirWriteWord(LongPtr ptr, uint16 offs, uint16 value) {
  uint8_t *dst = IndirPtr(ptr, offs);
  uint32 a = (*(uint32 *)&ptr & 0xffffff) + offs;
  uint16_t ram_addr_lo = (uint16_t)(a & 0x1ffff);
  uint16_t ram_addr_hi = (uint16_t)((a + 1) & 0x1ffff);
  if (ram_addr_lo >= 0xC800 && ram_addr_lo <= 0xCFFF) {
    debug_server_log_map16_write(ram_addr_lo, (uint8_t)value,
      (uint16_t)(*(uint32 *)&ptr & 0xffff),
      (uint8_t)((*(uint32 *)&ptr >> 16) & 0xff),
      offs);
  }
  if (ram_addr_hi >= 0xC800 && ram_addr_hi <= 0xCFFF) {
    debug_server_log_map16_write(ram_addr_hi, (uint8_t)(value >> 8),
      (uint16_t)(*(uint32 *)&ptr & 0xffff),
      (uint8_t)((*(uint32 *)&ptr >> 16) & 0xff),
      (uint16_t)(offs + 1));
  }
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
}

void RtlReset(int mode);
void RtlClearKeyLog();
void RtlStopReplay();
bool RtlIsReplayMode(void);

enum {
  kSaveLoad_Save = 1,
  kSaveLoad_Load = 2,
  kSaveLoad_Replay = 3,
};

void RtlSaveLoad(int cmd, int slot);
void RtlCheat(char c);
void RtlApuLock();
void RtlApuUnlock();
void RtlApuReset();
void RtlApuUpload(const uint8 *p);
void RtlRenderAudio(int16 *audio_buffer, int samples, int channels);
bool RtlRunFrame(uint32 inputs);
void RtlReadSram();
void RtlWriteSram();
void RtlSaveSnapshot(const char *filename, bool saving_with_bug);
bool RtlLoadSnapshot(const char *filename, bool replay);
uint8 RtlApuReadReg(int reg);
void RtlRecordPatchByte(const uint8 *value, int num);

void RtlUpdatePalette(const uint16 *src, int dst, int n);
uint16 *RtlGetVramAddr();
void RtlPpuWrite(uint16 addr, uint8 value);
void RtlPpuWriteTwice(uint16 addr, uint16 value);
void RtlApuWrite(uint16 adr, uint8 val);
void RtlEnableVirq(int line);


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