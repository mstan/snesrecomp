
#ifndef DMA_H
#define DMA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Dma Dma;

typedef void (*DmaVramNotifyHook)(uint8_t aBank, uint16_t aAdr,
                                  uint16_t vmadd, uint16_t size);
void dma_set_vram_notify_hook(DmaVramNotifyHook hook);

#include "snes.h"

typedef struct DmaChannel {
  uint8_t bAdr;
  uint16_t aAdr;
  uint8_t aBank;
  uint16_t size; // also indirect hdma adr
  uint8_t indBank; // hdma
  uint16_t tableAdr; // hdma
  uint8_t repCount; // hdma
  uint8_t unusedByte;
  bool dmaActive;
  bool hdmaActive;
  uint8_t mode;
  bool fixed;
  bool decrement;
  bool indirect; // hdma
  bool fromB;
  bool unusedBit;
  bool doTransfer; // hdma
  bool terminated; // hdma
  uint8_t offIndex;
} DmaChannel;

struct Dma {
  Snes* snes;
  /* Bitmask of channels whose HDMA was switched on part-way through a frame
   * and still owe their one-slot table initialization. See dma_doHdma().
   *
   * Declared BEFORE `channel` on purpose: dma_saveload() serializes from
   * `channel` to the end of the struct, so putting it here leaves the
   * savestate layout byte-identical. That is also the honest place for it --
   * it is transient within-frame sequencing, not guest-visible state. */
  uint8_t hdmaPendingInit;
  DmaChannel channel[8];
  uint32_t dmaTimer;
  bool dmaBusy;
};

Dma* dma_init(Snes* snes);
void dma_free(Dma* dma);
void dma_reset(Dma* dma);
uint8_t dma_read(Dma* dma, uint16_t adr); // 43x0-43xf
void dma_write(Dma* dma, uint16_t adr, uint8_t val); // 43x0-43xf
void dma_doDma(Dma* dma);
/* Per-scanline HDMA. A frame-model host owns these edges: call dma_initHdma()
 * once at the top of the visible field and dma_doHdma() before rendering each
 * scanline (FRAME_MODEL_HOSTS.md). */
void dma_initHdma(Dma* dma);
void dma_doHdma(Dma* dma);
uint64_t dma_hdmaMasterEstimate(Dma* dma); /* per-frame CPU-stall estimate */
bool dma_cycle(Dma* dma);
void dma_startDma(Dma* dma, uint8_t val, bool hdma);
void dma_saveload(Dma *dma, SaveLoadInfo *sli);

#endif
