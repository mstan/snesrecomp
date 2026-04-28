#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/snes.h"

// Recomp hardware-register routing — all internal-register reads and
// writes ($4200-$421F) dispatch to the snes_readReg / snes_writeReg
// impl borrowed from snes9x. Per-register timing semantics are
// documented in docs/VIRTUAL_HW_CONTRACT.md; the prior $4212 override
// here was replaced with an h-counter model inside snes_readReg so all
// callers get coherent state instead of a per-call toggle.

extern Snes *g_snes;
extern Dma *g_dma;

void recomp_write_internal_reg(uint16 reg, uint8 val) {
  snes_writeReg(g_snes, reg, val);
}

uint8 recomp_read_internal_reg(uint16 reg) {
  return snes_readReg(g_snes, reg);
}
