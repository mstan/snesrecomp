#include "recomp_state.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/snes.h"

// Direct hardware register handlers for the recomp path.
// These bypass the emulator bus (snes_write/snes_read) and dispatch
// directly to the appropriate subsystem.

extern uint8 g_ram[];
extern const uint8 *g_rom;
extern Snes *g_snes;
extern Ppu *g_ppu;
extern Dma *g_dma;

// --- Internal register state (owned by recomp, not g_snes) ---
static uint8_t  recomp_multiplyA;
static uint16_t recomp_multiplyResult;
static uint16_t recomp_divideA;
static uint16_t recomp_divideResult;
static bool     recomp_ppuLatch;

void recomp_hw_init(void) {
  recomp_multiplyA = 0xff;
  recomp_multiplyResult = 0xfe01;
  recomp_divideA = 0xffff;
  recomp_divideResult = 0x101;
  recomp_ppuLatch = false;
  g_recomp.wramAddr = 0;
}


// --- WRAM access port (0x2180-0x2183) ---

void recomp_write_wram_port(uint16 reg, uint8 val) {
  switch (reg) {
    case 0x2180:
      g_ram[g_recomp.wramAddr++] = val;
      g_recomp.wramAddr &= 0x1ffff;
      break;
    case 0x2181:
      g_recomp.wramAddr = (g_recomp.wramAddr & 0x1ff00) | val;
      break;
    case 0x2182:
      g_recomp.wramAddr = (g_recomp.wramAddr & 0x100ff) | (val << 8);
      break;
    case 0x2183:
      g_recomp.wramAddr = (g_recomp.wramAddr & 0x0ffff) | ((val & 1) << 16);
      break;
  }
}

uint8 recomp_read_wram_port(void) {
  uint8 ret = g_ram[g_recomp.wramAddr++];
  g_recomp.wramAddr &= 0x1ffff;
  return ret;
}

// --- Internal register writes (0x4200-0x421F) ---

void recomp_write_internal_reg(uint16 reg, uint8 val) {
  switch (reg) {
    case 0x4200:  // NMITIMEN
      g_snes->autoJoyRead = val & 0x1;
      g_snes->hIrqEnabled = val & 0x10;
      g_recomp.vIrqEnabled = (val & 0x20) != 0;
      g_snes->vIrqEnabled = g_recomp.vIrqEnabled;
      g_snes->nmiEnabled = val & 0x80;
      break;
    case 0x4201:  // WRIO
      if (!(val & 0x80) && recomp_ppuLatch)
        ppu_read(g_ppu, 0x37);
      recomp_ppuLatch = val & 0x80;
      break;
    case 0x4202:  // WRMPYA
      recomp_multiplyA = val;
      break;
    case 0x4203:  // WRMPYB - triggers multiply
      recomp_multiplyResult = recomp_multiplyA * val;
      break;
    case 0x4204:  // WRDIVL
      recomp_divideA = (recomp_divideA & 0xff00) | val;
      break;
    case 0x4205:  // WRDIVH
      recomp_divideA = (recomp_divideA & 0x00ff) | (val << 8);
      break;
    case 0x4206:  // WRDIVB - triggers divide
      if (val == 0) {
        recomp_divideResult = 0xffff;
        recomp_multiplyResult = recomp_divideA;
      } else {
        recomp_divideResult = recomp_divideA / val;
        recomp_multiplyResult = recomp_divideA % val;
      }
      break;
    case 0x4207:  // HTIMEL
      g_snes->hTimer = (g_snes->hTimer & 0x100) | val;
      break;
    case 0x4208:  // HTIMEH
      g_snes->hTimer = (g_snes->hTimer & 0x0ff) | ((val & 1) << 8);
      break;
    case 0x4209:  // VTIMEL
      g_recomp.vTimer = (g_recomp.vTimer & 0x100) | val;
      g_snes->vTimer = g_recomp.vTimer;
      break;
    case 0x420a:  // VTIMEH
      g_recomp.vTimer = (g_recomp.vTimer & 0x0ff) | ((val & 1) << 8);
      g_snes->vTimer = g_recomp.vTimer;
      break;
    case 0x420b:  // MDMAEN - DMA trigger
      dma_startDma(g_dma, val, false);
      while (dma_cycle(g_dma)) {}
      break;
    case 0x420c:  // HDMAEN
      dma_startDma(g_dma, val, true);
      break;
    case 0x420d:  // MEMSEL (fast/slow ROM - no-op for recomp)
      break;
  }
}

// --- Internal register reads (0x4200-0x421F) ---

static uint16_t SwapInputBits_Recomp(uint16_t x) {
  uint16_t r = 0;
  for (int i = 0; i < 16; i++, x >>= 1)
    r = r * 2 + (x & 1);
  return r;
}

uint8 recomp_read_internal_reg(uint16 reg) {
  switch (reg) {
    case 0x4210:  // RDNMI
    {
      // Bit 7 = NMI-pending, cleared on read (hardware-accurate).
      // Low 4 bits are 5A22 chip revision (commonly $02 for SNES).
      uint8 val = 0x02 | (g_snes->inNmi << 7);
      g_snes->inNmi = false;
      return val;
    }
    case 0x4211:  // TIMEUP
    {
      uint8 val = g_snes->inIrq << 7;
      g_snes->inIrq = false;
      return val;
    }
    case 0x4212:  // HVBJOY
    {
      // Bit 7 = VBlank. Bit 6 = HBlank. Our renderer runs a whole
      // scanline per ppu_runLine() call (no sub-scanline timing), so
      // HBlank isn't a real phase. ROM code that polls HVBJOY for the
      // HBlank bit (e.g. SMW's WaitForHBlank at $843B, which brackets
      // `BIT HVBJOY ; BVS -` then `BIT HVBJOY ; BVC -` to wait across
      // one HBlank-end + one HBlank-start transition) would deadlock
      // against a constant-value bit 6. Toggle bit 6 on every read so
      // the two-edge waits always make progress. The actual HBlank
      // duration (~25% of a scanline on real hardware) is elided; the
      // game sees "an HBlank occurred between reads." For counted waits
      // (LDY #N ; loop { WaitForHBlank ; DEY ; BNE }), Y decrements
      // correctly — the timing shortens but the semantic count is
      // preserved.
      static uint8 hblank_toggle;
      hblank_toggle ^= 0x40;
      return (g_snes->inVblank << 7) | hblank_toggle;
    }
    case 0x4213:  // RDIO
      return recomp_ppuLatch << 7;
    case 0x4214:  // RDDIVL
      return recomp_divideResult & 0xff;
    case 0x4215:  // RDDIVH
      return recomp_divideResult >> 8;
    case 0x4216:  // RDMPYL
      return recomp_multiplyResult & 0xff;
    case 0x4217:  // RDMPYH
      return recomp_multiplyResult >> 8;
    case 0x4218:  // JOY1L
      return SwapInputBits_Recomp(g_recomp.input1) & 0xff;
    case 0x4219:  // JOY1H
      return SwapInputBits_Recomp(g_recomp.input1) >> 8;
    case 0x421a:  // JOY2L
      return SwapInputBits_Recomp(g_recomp.input2) & 0xff;
    case 0x421b:  // JOY2H
      return SwapInputBits_Recomp(g_recomp.input2) >> 8;
    case 0x421c:
    case 0x421d:
    case 0x421e:
    case 0x421f:
      return 0;
    default:
      return 0;
  }
}
