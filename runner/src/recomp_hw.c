#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/snes.h"

// Direct hardware register handlers for the recomp path.
// These bypass the emulator bus (snes_write/snes_read) and dispatch
// directly to the appropriate subsystem, but operate on the SAME
// underlying snes/ppu/dma state — no parallel shadows.

extern uint8 g_ram[];
extern const uint8 *g_rom;
extern Snes *g_snes;
extern Ppu *g_ppu;
extern Dma *g_dma;

// --- Internal register writes (0x4200-0x421F) ---

void recomp_write_internal_reg(uint16 reg, uint8 val) {
  switch (reg) {
    case 0x4200:  // NMITIMEN
      g_snes->autoJoyRead = val & 0x1;
      g_snes->hIrqEnabled = val & 0x10;
      g_snes->vIrqEnabled = (val & 0x20) != 0;
      g_snes->nmiEnabled = val & 0x80;
      break;
    case 0x4201:  // WRIO
      if (!(val & 0x80) && g_snes->ppuLatch)
        ppu_read(g_ppu, 0x37);
      g_snes->ppuLatch = val & 0x80;
      break;
    case 0x4202:  // WRMPYA
      g_snes->multiplyA = val;
      break;
    case 0x4203:  // WRMPYB - triggers multiply
      g_snes->multiplyResult = g_snes->multiplyA * val;
      break;
    case 0x4204:  // WRDIVL
      g_snes->divideA = (g_snes->divideA & 0xff00) | val;
      break;
    case 0x4205:  // WRDIVH
      g_snes->divideA = (g_snes->divideA & 0x00ff) | (val << 8);
      break;
    case 0x4206:  // WRDIVB - triggers divide
      if (val == 0) {
        g_snes->divideResult = 0xffff;
        g_snes->multiplyResult = g_snes->divideA;
      } else {
        g_snes->divideResult = g_snes->divideA / val;
        g_snes->multiplyResult = g_snes->divideA % val;
      }
      break;
    case 0x4207:  // HTIMEL
      g_snes->hTimer = (g_snes->hTimer & 0x100) | val;
      break;
    case 0x4208:  // HTIMEH
      g_snes->hTimer = (g_snes->hTimer & 0x0ff) | ((val & 1) << 8);
      break;
    case 0x4209:  // VTIMEL
      g_snes->vTimer = (g_snes->vTimer & 0x100) | val;
      break;
    case 0x420a:  // VTIMEH
      g_snes->vTimer = (g_snes->vTimer & 0x0ff) | ((val & 1) << 8);
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
      return g_snes->ppuLatch << 7;
    case 0x4214:  // RDDIVL
      return g_snes->divideResult & 0xff;
    case 0x4215:  // RDDIVH
      return g_snes->divideResult >> 8;
    case 0x4216:  // RDMPYL
      return g_snes->multiplyResult & 0xff;
    case 0x4217:  // RDMPYH
      return g_snes->multiplyResult >> 8;
    case 0x4218:  // JOY1L
      return SwapInputBits(g_snes->input1_currentState) & 0xff;
    case 0x4219:  // JOY1H
      return SwapInputBits(g_snes->input1_currentState) >> 8;
    case 0x421a:  // JOY2L
      return SwapInputBits(g_snes->input2_currentState) & 0xff;
    case 0x421b:  // JOY2H
      return SwapInputBits(g_snes->input2_currentState) >> 8;
    case 0x421c:
    case 0x421d:
    case 0x421e:
    case 0x421f:
      return 0;
    default:
      return 0;
  }
}
