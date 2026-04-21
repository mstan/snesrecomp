#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/snes.h"

// Recomp hardware-register routing. All internal-register reads and
// writes ($4200-$421F) dispatch to the same snes_writeReg / snes_readReg
// the emulator uses. There is ONE override: $4212 HVBJOY read, because
// recomp has no cycle-stepping CPU advancing hPos — an unchanging HBlank
// bit would deadlock SMW's WaitForHBlank. We toggle bit 6 per read to
// guarantee the two-edge poll sees progress.

extern Snes *g_snes;
extern Dma *g_dma;

void recomp_write_internal_reg(uint16 reg, uint8 val) {
  snes_writeReg(g_snes, reg, val);
}

uint8 recomp_read_internal_reg(uint16 reg) {
  if (reg == 0x4212) {
    // Recomp lacks a cycle-accurate CPU to advance hPos per scanline,
    // so the emulator's HBlank-bit computation would pin bit 6 and
    // deadlock any ROM that polls for HBlank edges (e.g. SMW's
    // WaitForHBlank at $843B: `BIT HVBJOY ; BVS -` then
    // `BIT HVBJOY ; BVC -` waits across one HBlank-end + one
    // HBlank-start transition). Toggling bit 6 per read makes both
    // edge waits make progress.
    static uint8 hblank_toggle;
    hblank_toggle ^= 0x40;
    return (g_snes->inVblank << 7) | hblank_toggle;
  }
  return snes_readReg(g_snes, reg);
}
