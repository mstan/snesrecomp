
#ifndef SNES_H
#define SNES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Snes Snes;

#include "cpu.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "cart.h"
#include "saveload.h"

struct Snes {
  Cpu* cpu;
  Apu* apu;
  Ppu* ppu;
  Dma* dma;
  Cart* cart;
  uint16 input1_currentState;
  uint16 input2_currentState;
  bool disableRender;

  // ram data port ($2180-$2183)
  uint32_t ramAdr;
  uint8_t *ram;

  // --- saveload blob starts here (hPos .. divideResult) ---
  uint16_t hPos;
  uint16_t vPos;
  double apuCatchupCycles;
  // nmi / irq
  bool hIrqEnabled;
  bool vIrqEnabled;
  bool nmiEnabled;
  uint16_t hTimer;
  uint16_t vTimer;
  bool inNmi;
  bool inIrq;
  bool inVblank;
  // joypad
  bool autoJoyRead;
  uint16_t autoJoyTimer;
  bool ppuLatch;
  // multiplication/division
  uint8_t multiplyA;
  uint16_t multiplyResult;
  uint16_t divideA;
  uint16_t divideResult;
};

Snes* snes_init(uint8_t *ram);
void snes_free(Snes* snes);
void snes_reset(Snes* snes, bool hard);
// used by dma, cpu
uint8_t snes_readBBus(Snes* snes, uint8_t adr);
void snes_writeBBus(Snes* snes, uint8_t adr, uint8_t val);
uint8_t snes_read(Snes* snes, uint32_t adr);
void snes_write(Snes* snes, uint32_t adr, uint8_t val);
uint8_t snes_readReg(Snes* snes, uint16_t adr);
void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val);
uint16_t SwapInputBits(uint16_t x);


// snes_other.c functions:

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);
void snes_saveload(Snes *snes, SaveLoadInfo *sli);
void snes_catchupApu(Snes *snes);

extern int snes_frame_counter;
#endif
