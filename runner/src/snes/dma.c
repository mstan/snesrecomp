
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#include "dma.h"
#include "snes.h"
#include "ppu.h"
#include "sdd1.h"
#include "../debug_server.h"

extern Ppu *g_ppu;
static DmaVramNotifyHook g_vram_notify_hook;

/* Debug logging gate: set SNESRECOMP_DMA_DEBUG=1 to enable the DMA config /
 * CGRAM / S-DD1 arm trace. Off by default — the trace fires for every DMA
 * the game starts and slows real runs down. */
static int dma_debug_enabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("SNESRECOMP_DMA_DEBUG") ? 1 : 0;
    return v;
}

void dma_set_vram_notify_hook(DmaVramNotifyHook hook) {
  g_vram_notify_hook = hook;
}

static const int bAdrOffsets[8][4] = {
  {0, 0, 0, 0},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1},
  {0, 1, 2, 3},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1}
};

static const int transferLength[8] = {
  1, 2, 2, 4, 4, 4, 2, 4
};

static void dma_transferByte(Dma* dma, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB, int channel);

Dma* dma_init(Snes* snes) {
  /* calloc (not malloc): dma_saveload hashes the raw struct region incl.
   * DmaChannel padding bytes; leaving them uninitialized makes save-states
   * (and the co-sim state hash) nondeterministic run-to-run. */
  Dma* dma = calloc(1, sizeof(Dma));
  dma->snes = snes;
  return dma;
}

void dma_free(Dma* dma) {
  free(dma);
}

void dma_reset(Dma* dma) {
  dma->hdmaPendingInit = 0;
  for(int i = 0; i < 8; i++) {
    dma->channel[i].bAdr = 0xff;
    dma->channel[i].aAdr = 0xffff;
    dma->channel[i].aBank = 0xff;
    dma->channel[i].size = 0xffff;
    dma->channel[i].indBank = 0xff;
    dma->channel[i].tableAdr = 0xffff;
    dma->channel[i].repCount = 0xff;
    dma->channel[i].unusedByte = 0xff;
    dma->channel[i].dmaActive = false;
    dma->channel[i].hdmaActive = false;
    dma->channel[i].mode = 7;
    dma->channel[i].fixed = true;
    dma->channel[i].decrement = true;
    dma->channel[i].indirect = true;
    dma->channel[i].fromB = true;
    dma->channel[i].unusedBit = true;
    dma->channel[i].doTransfer = false;
    dma->channel[i].terminated = false;
    dma->channel[i].offIndex = 0;
  }
  dma->dmaTimer = 0;
  dma->dmaBusy = false;
}

void dma_saveload(Dma *dma, SaveLoadInfo *sli) {
  sli->func(sli, &dma->channel, sizeof(*dma) - offsetof(Dma, channel));
}

uint8_t dma_read(Dma* dma, uint16_t adr) {
  uint8_t c = (adr & 0x70) >> 4;
  switch(adr & 0xf) {
    case 0x0: {
      uint8_t val = dma->channel[c].mode;
      val |= dma->channel[c].fixed << 3;
      val |= dma->channel[c].decrement << 4;
      val |= dma->channel[c].unusedBit << 5;
      val |= dma->channel[c].indirect << 6;
      val |= dma->channel[c].fromB << 7;
      return val;
    }
    case 0x1: {
      return dma->channel[c].bAdr;
    }
    case 0x2: {
      return dma->channel[c].aAdr & 0xff;
    }
    case 0x3: {
      return dma->channel[c].aAdr >> 8;
    }
    case 0x4: {
      return dma->channel[c].aBank;
    }
    case 0x5: {
      return dma->channel[c].size & 0xff;
    }
    case 0x6: {
      return dma->channel[c].size >> 8;
    }
    case 0x7: {
      return dma->channel[c].indBank;
    }
    case 0x8: {
      return dma->channel[c].tableAdr & 0xff;
    }
    case 0x9: {
      return dma->channel[c].tableAdr >> 8;
    }
    case 0xa: {
      return dma->channel[c].repCount;
    }
    case 0xb:
    case 0xf: {
      return dma->channel[c].unusedByte;
    }
    default: {
      /* Soft for v2 boot: data-as-code reads occasionally hit invalid
       * DMA register offsets (e.g. \$430C/\$430E that don't exist).
       * Real fix is upstream — for now return 0 so boot continues. */
      return 0;
    }
  }
}

void dma_write(Dma* dma, uint16_t adr, uint8_t val) {
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
  if (dma_debug_enabled())
    fprintf(stderr, "[dma] write $43%02x=%02x (ch=%d)\n", adr & 0xff, val,
            (adr & 0x70) >> 4);
#endif
  uint8_t c = (adr & 0x70) >> 4;
  switch(adr & 0xf) {
    case 0x0: {
      dma->channel[c].mode = val & 0x7;
      dma->channel[c].fixed = val & 0x8;
      dma->channel[c].decrement = val & 0x10;
      dma->channel[c].unusedBit = val & 0x20;
      dma->channel[c].indirect = val & 0x40;
      dma->channel[c].fromB = val & 0x80;
      break;
    }
    case 0x1: {
      dma->channel[c].bAdr = val;
      break;
    }
    case 0x2: {
      dma->channel[c].aAdr = (dma->channel[c].aAdr & 0xff00) | val;
      if (dma_debug_enabled()) {
        static int dma_cfg2_log = 0;
        if (dma_cfg2_log < 40) {
          fprintf(stderr, "[DMA_CFG] ch%d $43%01X(aAdr_lo)=$%02X -> aAdr=$%04X\n",
                  c, adr & 0xf, val, dma->channel[c].aAdr);
          dma_cfg2_log++;
        }
      }
      break;
    }
    case 0x3: {
      dma->channel[c].aAdr = (dma->channel[c].aAdr & 0xff) | (val << 8);
      if (dma_debug_enabled()) {
        static int dma_cfg3_log = 0;
        if (dma_cfg3_log < 40) {
          fprintf(stderr, "[DMA_CFG] ch%d $43%01X(aAdr_hi)=$%02X -> aAdr=$%04X\n",
                  c, adr & 0xf, val, dma->channel[c].aAdr);
          dma_cfg3_log++;
        }
      }
      break;
    }
    case 0x4: {
      dma->channel[c].aBank = val;
      if (dma_debug_enabled()) {
        static int dma_cfg_log = 0;
        if (dma_cfg_log < 40) {
          fprintf(stderr, "[DMA_CFG] ch%d $43%01X(bank)=$%02X (was $%02X) src=$%02X:%04X\n",
                  c, adr & 0xf, val, dma->channel[c].aBank, val, dma->channel[c].aAdr);
          dma_cfg_log++;
        }
      }
      break;
    }
    case 0x5: {
      dma->channel[c].size = (dma->channel[c].size & 0xff00) | val;
      break;
    }
    case 0x6: {
      dma->channel[c].size = (dma->channel[c].size & 0xff) | (val << 8);
      break;
    }
    case 0x7: {
      dma->channel[c].indBank = val;
      break;
    }
    case 0x8: {
      dma->channel[c].tableAdr = (dma->channel[c].tableAdr & 0xff00) | val;
      break;
    }
    case 0x9: {
      dma->channel[c].tableAdr = (dma->channel[c].tableAdr & 0xff) | (val << 8);
      break;
    }
    case 0xa: {
      dma->channel[c].repCount = val;
      break;
    }
    case 0xb:
    case 0xf: {
      dma->channel[c].unusedByte = val;
      break;
    }
    default: {
      break;
    }
  }
}

extern bool g_fail;

void dma_doDma(Dma* dma) {
  if(dma->dmaTimer > 0) {
    dma->dmaTimer -= 2;
    return;
  }
  // figure out first channel that is active
  int i = 0;
  for(i = 0; i < 8; i++) {
    if(dma->channel[i].dmaActive) {
      break;
    }
  }
  if(i == 8) {
    // no active channels
    dma->dmaBusy = false;
    return;
  }

  /* This heuristic was written for LoROM, where a high bank with A < $8000
   * is usually not ROM. HiROM maps banks $C0-$FF across the full address
   * range, so sources such as DKC2's $F8:0FA6 are ordinary cartridge data. */
  if (!dma->channel[i].fromB && dma->snes && dma->snes->cart &&
      (dma->snes->cart->type == CART_LOROM ||
       dma->snes->cart->type == CART_DSP1) &&
      (dma->channel[i].aBank & 0x80) &&
      !(dma->channel[i].aAdr & 0x8000) && !g_fail) {
    printf("Warning! DMA from addr 0x%x\n", dma->channel[i].aBank << 16 | dma->channel[i].aAdr);
    g_fail = true;
  }

  // do channel i
  dma_transferByte(
    dma, dma->channel[i].aAdr, dma->channel[i].aBank,
    dma->channel[i].bAdr + bAdrOffsets[dma->channel[i].mode][dma->channel[i].offIndex++], dma->channel[i].fromB, i
  );
  dma->channel[i].offIndex &= 3;
  dma->dmaTimer += 6; // 8 cycles for each byte taken, -2 for this cycle
  if(!dma->channel[i].fixed) {
    dma->channel[i].aAdr += dma->channel[i].decrement ? -1 : 1;
  }
  dma->channel[i].size--;
  if(dma->channel[i].size == 0) {
    dma->channel[i].offIndex = 0; // reset offset index
    dma->channel[i].dmaActive = false;
    dma->dmaTimer += 8; // 8 cycle overhead per channel
  }
}

#ifdef SNESRECOMP_INTERP_PROFILE
#include <time.h>
uint64_t dma_prof_bytes = 0;
double dma_prof_ms = 0.0;
#endif
static void dma_transferByte(Dma* dma, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB, int channel) {
#ifdef SNESRECOMP_INTERP_PROFILE
  clock_t _t0 = clock();
  { extern uint64_t dma_prof_bytes; extern double dma_prof_ms;
    dma_prof_bytes++; }
#endif
  if (fromB) {
    snes_write(dma->snes, (aBank << 16) | aAdr, snes_readBBus(dma->snes, bAdr));
  } else {
    uint8_t val;
    Cart* cart = dma->snes->cart;
    if (cart && cart->type == CART_SDD1 && cart->sdd1 && channel >= 0 && channel < 8) {
      if (sdd1_dma_active(cart->sdd1, channel)) {
        val = sdd1_dma_get_byte(cart->sdd1, channel);
        if (dma_debug_enabled()) {
          static int sdd1_vram_log = 0;
          if (sdd1_vram_log < 30) {
            fprintf(stderr, "[SDD1_DMA] ch%d bAdr=$%02X val=$%02X\n",
                    channel, bAdr, val);
            sdd1_vram_log++;
          }
        }
        debug_server_on_reg_write((uint16_t)(0x2100u + bAdr), val);
        snes_writeBBus(dma->snes, bAdr, val);
        return;
      }
    }
    val = snes_read(dma->snes, (aBank << 16) | aAdr);
    if (dma_debug_enabled() && (bAdr == 0x21 || bAdr == 0x22)) {
      static int cgram_dma_log = 0;
      if (cgram_dma_log < 30) {
        fprintf(stderr, "[DMA_CGRAM] ch%d bAdr=$%02X val=$%02X src=%02X:%04X fullAddr=%06X\n",
                channel, bAdr, val, aBank, aAdr, (aBank << 16) | aAdr);
        cgram_dma_log++;
      }
    }
    debug_server_on_reg_write((uint16_t)(0x2100u + bAdr), val);
    snes_writeBBus(dma->snes, bAdr, val);
  }
#ifdef SNESRECOMP_INTERP_PROFILE
  { extern uint64_t dma_prof_bytes; extern double dma_prof_ms;
    dma_prof_ms += 1000.0 * ((double)(clock() - _t0)) / CLOCKS_PER_SEC;
  }
#endif
}

/* HDMA uses a table descriptor per active channel. It is separate from the
 * regular-DMA cycle engine because the host invokes it at scanline edges. */
uint64_t dma_hdmaMasterEstimate(Dma* dma) {
  static const uint8_t bytesPerUnit[8] = {1, 2, 2, 4, 4, 4, 2, 4};
  uint64_t perLine = 0;
  bool any = false;
  for (int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if (!ch->hdmaActive) continue;
    any = true;
    perLine += 8u + 8u * bytesPerUnit[ch->mode & 7];
    if (ch->indirect) perLine += 16u;
  }
  return any ? (18u + perLine) * 224u + 128u : 0;
}

static void dma_hdmaLoadDescriptor(Dma* dma, DmaChannel* ch) {
  ch->repCount = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
  ch->terminated = ch->repCount == 0;
  if (ch->indirect && !ch->terminated) {
    ch->size = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
    ch->size |= snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++) << 8;
  }
  ch->doTransfer = !ch->terminated;
}

void dma_initHdma(Dma* dma) {
  for (int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if (!ch->hdmaActive) continue;
    dma->hdmaPendingInit &= (uint8_t)~(1u << i);
    ch->tableAdr = ch->aAdr;
    dma_hdmaLoadDescriptor(dma, ch);
    ch->offIndex = 0;
  }
}

void dma_primeHdmaFirstLine(Dma* dma) {
  for (int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if (!ch->hdmaActive || ch->terminated || !ch->doTransfer) continue;
    DmaChannel save = *ch;
    int len = transferLength[ch->mode & 7];
    for (int j = 0; j < len; j++) {
      uint8_t bAdr = (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode & 7][j]);
      if (ch->indirect)
        dma_transferByte(dma, ch->size++, ch->indBank, bAdr, false, i);
      else
        dma_transferByte(dma, ch->tableAdr++, ch->aBank, bAdr, false, i);
    }
    *ch = save;
  }
}

void dma_doHdma(Dma* dma) {
  for (int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if (!ch->hdmaActive) continue;
    if (dma->hdmaPendingInit & (1u << i)) {
      dma->hdmaPendingInit &= (uint8_t)~(1u << i);
      ch->tableAdr = ch->aAdr;
      dma_hdmaLoadDescriptor(dma, ch);
      ch->offIndex = 0;
      continue;
    }
    if (ch->terminated) continue;

    if (ch->doTransfer) {
      int len = transferLength[ch->mode & 7];
      for (int j = 0; j < len; j++) {
        uint8_t bAdr = (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode & 7][j]);
        if (ch->indirect)
          dma_transferByte(dma, ch->size++, ch->indBank, bAdr, false, i);
        else
          dma_transferByte(dma, ch->tableAdr++, ch->aBank, bAdr, false, i);
      }
    }

    ch->repCount--;
    ch->doTransfer = (ch->repCount & 0x80) != 0;
    if ((ch->repCount & 0x7f) == 0)
      dma_hdmaLoadDescriptor(dma, ch);
  }
}

/* ── HDMA ────────────────────────────────────────────────────────────────
 *
 * dma_startDma(..., hdma=true) has always set channel[i].hdmaActive from
 * $420C, and DmaChannel has carried tableAdr / repCount / doTransfer /
 * terminated / indBank since forever — but nothing ever consumed any of it.
 * There was no HDMA transfer engine in the tree at all, so a game's per-
 * scanline register stream simply never happened.
 *
 * Measured on Gundam Wing Endless Duel: through the intro cutscene, channel 1
 * is HDMA-active onto $212C (TM, main-screen layer enable), rewriting which
 * BG layers are on per scanline. That is the letterbox — BG off on the top
 * and bottom bands, on in the middle, with OBJ left enabled so the characters
 * still draw over the bars. Without the engine every line got one TM value
 * and the whole effect was lost.
 *
 * Standard algorithm: dma_initHdma() reloads each enabled channel's table
 * pointer at the top of the field; dma_doHdma() runs once per scanline.
 * `size` doubles as the indirect address, as the struct comment notes. */

/* Estimated master clocks one frame of HDMA steals from the CPU.
 *
 * Hardware pauses the CPU during every active channel's per-line transfer:
 * ~18 clocks of per-line overhead when any channel is live, plus per channel
 * 8 clocks of address work and 8 per byte moved (1/2/2/4/4/4/2/4 bytes for
 * modes 0-7), plus 16 more when the channel reloads an indirect address.
 * With the six-channel gradient/scroll setup this title runs on its menu and
 * VS screens that is ~170 clocks x 224 lines = ~10.7%% of the frame -- more
 * than DRAM refresh -- and it was never charged: measured, our menu lag
 * blocks ran one frame short of Mesen's (7 vs 8) while the HDMA-off loading
 * screens matched exactly. The phase error that mispairs the sprite-table
 * and tile-art updates at scene entry rides on exactly that deficit.
 *
 * An estimate from the live channel state at frame start; terminated-early
 * tables overcharge slightly, which is the conservative side. */
uint64_t dma_hdmaMasterEstimate(Dma* dma) {
  static const uint8_t bytesPerUnit[8] = {1, 2, 2, 4, 4, 4, 2, 4};
  uint64_t perLine = 0;
  int any = 0;
  for (int i = 0; i < 8; i++) {
    DmaChannel* c = &dma->channel[i];
    if (!c->hdmaActive) continue;
    any = 1;
    perLine += 8u + 8u * bytesPerUnit[c->mode & 7];
    if (c->indirect) perLine += 16u;
  }
  if (!any) return 0;
  return (18u + perLine) * 224u + 128u /* per-frame init */;
}

void dma_initHdma(Dma* dma) {
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(!ch->hdmaActive) continue;
    /* A start-of-frame init supersedes any pending mid-frame one. */
    dma->hdmaPendingInit &= (uint8_t)~(1u << i);
    ch->tableAdr = ch->aAdr;
    ch->repCount = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
    ch->terminated = (ch->repCount == 0);
    if(ch->indirect) {
      ch->size = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
      ch->size |= snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++) << 8;
    }
    ch->doTransfer = true;
    ch->offIndex = 0;
  }
}

void dma_doHdma(Dma* dma) {
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(!ch->hdmaActive) continue;

    /* A channel switched on part-way through a frame spends its first HDMA
     * slot loading the table header, and only transfers from the slot after.
     * Measured against Mesen on Gundam Wing's pre-fight screen, which enables
     * $420C at line 21 and whose first HDMA write of $212C lands on line 23:
     *
     *     enable L21  ->  init L22  ->  first transfer L23
     *
     * Initializing and transferring in the same slot put every band edge one
     * scanline early (L22/54/102/134/182 against hardware's
     * L23/55/103/135/183) -- the whole raster split shifted up a pixel. */
    if(dma->hdmaPendingInit & (1u << i)) {
      dma->hdmaPendingInit &= (uint8_t)~(1u << i);
      ch->tableAdr = ch->aAdr;
      ch->repCount = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
      ch->terminated = (ch->repCount == 0);
      if(ch->indirect) {
        ch->size = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
        ch->size |= snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++) << 8;
      }
      ch->doTransfer = true;
      ch->offIndex = 0;
      continue;             /* this slot was the init; no transfer yet */
    }
    if(ch->terminated) continue;

    if(ch->doTransfer) {
      int len = transferLength[ch->mode];
      for(int j = 0; j < len; j++) {
        uint8_t b = (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode][j]);
        if(ch->indirect) {
          dma_transferByte(dma, ch->size++, ch->indBank, b, false);
        } else {
          dma_transferByte(dma, ch->tableAdr++, ch->aBank, b, false);
        }
      }
    }

    ch->repCount--;
    /* Bit 7 of the line-count byte is the "continue" flag: keep transferring
     * on each of the next (count & 0x7F) lines rather than only the first. */
    ch->doTransfer = (ch->repCount & 0x80) != 0;
    if((ch->repCount & 0x7f) == 0) {
      ch->repCount = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
      if(ch->indirect) {
        ch->size = snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++);
        ch->size |= snes_read(dma->snes, (ch->aBank << 16) | ch->tableAdr++) << 8;
      }
      ch->terminated = (ch->repCount == 0);
      ch->doTransfer = true;
    }
  }
}

bool dma_cycle(Dma* dma) {
  if(dma->dmaBusy) {
    dma_doDma(dma);
    return true;
  }
  return false;
}

void dma_startDma(Dma* dma, uint8_t val, bool hdma) {
  for(int i = 0; i < 8; i++) {
    if(hdma) {
      /* Only a channel going from off to on owes an initialization; rewriting
       * $420C with a channel already running must not restart its table. */
      bool now_on = (val & (1 << i)) != 0;
      if(now_on && !dma->channel[i].hdmaActive)
        dma->hdmaPendingInit |= (uint8_t)(1u << i);
      else if(!now_on)
        dma->hdmaPendingInit &= (uint8_t)~(1u << i);
      dma->channel[i].hdmaActive = now_on;
    } else {
      dma->channel[i].dmaActive = val & (1 << i);
    }
  }
  if(!hdma) {
    dma->dmaBusy = val;
    dma->dmaTimer += dma->dmaBusy ? 16 : 0; // 12-24 cycle overhead for entire dma transfer
    /* Diagnostic: log ALL DMA triggers with source addresses */
    if (dma_debug_enabled() && dma->snes && dma->snes->cart &&
        dma->snes->cart->type == CART_SDD1) {
      static int dma_sdd1_log = 0;
      if (dma_sdd1_log < 20) {
        for (int j = 0; j < 8; j++) {
          if (val & (1 << j)) {
            const DmaChannel *ch = &dma->channel[j];
            fprintf(stderr, "[DMA_SDD1_ARM] ch%d mode=%d bAdr=$%02X src=%02X:%04X size=$%04X r4800=$%02X r4801=$%02X\n",
                    j, ch->mode, ch->bAdr, ch->aBank, ch->aAdr, ch->size,
                    dma->snes->cart->sdd1->r4800, dma->snes->cart->sdd1->r4801);
            dma_sdd1_log++;
          }
        }
      }
    }
    /* S-DD1: arm a decompression session for every channel that is DMAing
     * from the chip's C0-FF MMC window with both hard ($4800) and soft
     * ($4801) enable set for that channel. The session produces the
     * decompressed bytes in dma_transferByte. */
    if (dma->snes && dma->snes->cart &&
        dma->snes->cart->type == CART_SDD1 && dma->snes->cart->sdd1) {
      for (int i = 0; i < 8; i++) {
        if (!(val & (1 << i)))
          continue;
        const DmaChannel *ch = &dma->channel[i];
        if (ch->fromB)
          continue;
        if ((ch->aBank & 0xFF) < 0xC0)
          continue;
        sdd1_dma_init(dma->snes->cart->sdd1, i,
                      ((uint32_t)ch->aBank << 16) | ch->aAdr, ch->size);
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
        if (dma_debug_enabled() && sdd1_dma_active(dma->snes->cart->sdd1, i)) {
          fprintf(stderr, "[sdd1] DMA session armed ch=%d src=%06x size=%u\n",
                  i, ((uint32_t)ch->aBank << 16) | ch->aAdr, ch->size);
        } else if (dma_debug_enabled()) {
          fprintf(stderr, "[sdd1] DMA arm REJECTED ch=%d src=%06x size=%u\n",
                  i, ((uint32_t)ch->aBank << 16) | ch->aAdr, ch->size);
        }
#endif
      }
    }
    if (val && g_ppu) {
      for (int i = 0; i < 8; i++) {
        if (!(val & (1 << i)))
          continue;
        const DmaChannel *ch = &dma->channel[i];
        if (ch->bAdr == 0x18 || ch->bAdr == 0x19) {
          if (g_vram_notify_hook)
            g_vram_notify_hook(ch->aBank, ch->aAdr, g_ppu->vramPointer, ch->size);
        }
        /* Log ALL CGRAM DMA and VRAM DMA */
        if (dma_debug_enabled() && (ch->bAdr == 0x18 || ch->bAdr == 0x19 ||
            ch->bAdr == 0x21 || ch->bAdr == 0x22)) {
          static int dma_all_log = 0;
          if (dma_all_log < 50) {
            fprintf(stderr, "[DMA_CH%d] mode=%d bAdr=$%02X src=%02X:%04X size=$%04X vmadd=$%04X fixed=%d dec=%d fromB=%d\n",
                    i, ch->mode, ch->bAdr, ch->aBank, ch->aAdr, ch->size,
                    g_ppu->vramPointer, ch->fixed, ch->decrement, ch->fromB);
            dma_all_log++;
          }
        }
      }
    }
  }
}
