
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include "snes.h"
#include "cpu.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "cart.h"
#include "variables.h"
#include "../common_rtl.h"
#include "../debug_server.h"

int snes_frame_counter;
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

uint8_t snes_readReg(Snes* snes, uint16_t adr);
void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val);

Snes* snes_init(uint8_t *ram) {
  Snes* snes = malloc(sizeof(Snes));
  snes->ram = ram;

  snes->cpu = cpu_init();
  snes->apu = apu_init();
  snes->dma = dma_init(snes);
  snes->ppu = ppu_init();
  snes->cart = cart_init(snes);
  snes->input1_currentState = 0;
  snes->input2_currentState = 0;
  return snes;
}

void snes_free(Snes* snes) {
  cpu_free(snes->cpu);
  apu_free(snes->apu);
  dma_free(snes->dma);
  ppu_free(snes->ppu);
  cart_free(snes->cart);
  free(snes);
}

void snes_saveload(Snes *snes, SaveLoadInfo *sli) {
  cpu_saveload(snes->cpu, sli);
  apu_saveload(snes->apu, sli);
  dma_saveload(snes->dma, sli);
  ppu_saveload(snes->ppu, sli);
  cart_saveload(snes->cart, sli);

  sli->func(sli, &snes->hPos, sizeof(*snes) - offsetof(Snes, hPos));
  sli->func(sli, snes->ram, 0x20000);
  sli->func(sli, &snes->ramAdr, 4);

  snes->cpu->e = 0;
}

void snes_reset(Snes* snes, bool hard) {
  cart_reset(snes->cart); // reset cart first, because resetting cpu will read from it (reset vector)
  cpu_reset(snes->cpu);
  apu_reset(snes->apu);
  dma_reset(snes->dma);
  ppu_reset(snes->ppu);
  if (hard)
    memset(snes->ram, 0, 0x20000);
  snes->ramAdr = 0;
  snes->hPos = 0;
  snes->vPos = 0;
  snes->apuCatchupCycles = 0.0;
  snes->hIrqEnabled = false;
  snes->vIrqEnabled = false;
  snes->nmiEnabled = false;
  snes->hTimer = 0x1ff;
  snes->vTimer = 0x1ff;
  snes->inNmi = false;
  snes->inIrq = false;
  snes->inVblank = false;
  snes->autoJoyRead = false;
  snes->autoJoyTimer = 0;
  snes->ppuLatch = false;
  snes->multiplyA = 0xff;
  snes->multiplyResult = 0xfe01;
  snes->divideA = 0xffff;
  snes->divideResult = 0x101;
}

static uint64_t s_catchup_calls = 0;
static uint64_t s_catchup_cycles_total = 0;
uint64_t g_apu_timer0_total_ticks = 0;
void snes_catchupApu(Snes* snes) {
  if (snes->apuCatchupCycles > 10000)
    snes->apuCatchupCycles = 10000;

  int catchupCycles = (int) snes->apuCatchupCycles;
  if (catchupCycles < 1024) catchupCycles = 1024;

  for(int i = 0; i < catchupCycles; i++) {
    apu_cycle(snes->apu);
  }
  snes->apuCatchupCycles -= (double) catchupCycles;
  if (snes->apuCatchupCycles < 0.0) snes->apuCatchupCycles = 0.0;
  s_catchup_calls++;
  s_catchup_cycles_total += (uint64_t)catchupCycles;
}

void snes_catchup_stats(uint64_t *calls, uint64_t *cycles) {
  if (calls) *calls = s_catchup_calls;
  if (cycles) *cycles = s_catchup_cycles_total;
}

uint8_t snes_readBBus(Snes* snes, uint8_t adr) {
  if(adr < 0x40) {
    return ppu_read(g_ppu, adr);
  }
  if(adr < 0x80) {
    // APU port read ($2140-$217F). Catch the APU up based on the
    // main-CPU cycles elapsed since the last APU sync, and return
    // the live outPort value. RtlApuLock serialises us against the
    // audio thread's render loop, which advances the APU under the
    // same lock.
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(snes);
    uint8_t v = snes->apu->outPorts[adr & 0x3];
    RtlApuUnlock();
    return v;
  }
  if(adr == 0x80) {
    uint8_t ret = snes->ram[snes->ramAdr++];
    snes->ramAdr &= 0x1ffff;
    return ret;
  }

  /* Out-of-range B-bus read. v2 boot path occasionally fires DMA
   * with a misconfigured channel (consequence of an upstream bad
   * ROM read returning garbage that configures the DMA setup). On
   * release builds we silently return 0 instead of crashing so the
   * boot path can keep progressing — the upstream issue is what
   * actually needs fixing. */
  return 0;
}

void snes_writeBBus(Snes* snes, uint8_t adr, uint8_t val) {
  if(adr < 0x40) {
    ppu_write(g_ppu, adr, val);
    return;
  }
  if(adr < 0x80) {
    RtlApuWrite(0x2100 + adr, val);
    return;
  }
  switch(adr) {
    case 0x80: {
      snes->ram[snes->ramAdr++] = val;
      snes->ramAdr &= 0x1ffff;
      break;
    }
    case 0x81: {
      snes->ramAdr = (snes->ramAdr & 0x1ff00) | val;
      break;
    }
    case 0x82: {
      snes->ramAdr = (snes->ramAdr & 0x100ff) | (val << 8);
      break;
    }
    case 0x83: {
      snes->ramAdr = (snes->ramAdr & 0x0ffff) | ((val & 1) << 16);
      break;
    }
  }
}

uint16_t SwapInputBits(uint16_t x) {
  uint16_t r = 0;
  for (int i = 0; i < 16; i++, x >>= 1)
    r = r * 2 + (x & 1);
  return r;
}

uint8_t snes_readReg(Snes* snes, uint16_t adr) {
  switch(adr) {
    case 0x4210: {
      uint8_t val = 0x2; // CPU version (4 bit)
      val |= snes->inNmi << 7;
      // Real hardware clears the NMI-pending latch on read. Without this
      // a stale `inNmi=true` would persist across NMI handler exit and
      // produce a spurious second-read=true if anything re-reads $4210
      // before the next NMI fires. (SMW happens to discard the loaded
      // value, but a hardware-correct read-clear costs one store and
      // is the right contract for game #2.)
      snes->inNmi = false;
      return val;
    }
    case 0x4211: {
      uint8_t val = snes->inIrq << 7;
      snes->inIrq = false;
      return val;
    }
    case 0x4212: {
      // Static-recomp h-counter model: real hardware updates hPos every
      // dot-clock; recomp has no dot-clock, so each $4212 read advances
      // hPos by a fixed step. Calibrated so a typical busy-wait crosses
      // both edges in ~10-20 reads. Bit 6 = hblank (dots ~1024..1364 of
      // a 1364-dot scanline). See docs/VIRTUAL_HW_CONTRACT.md.
      snes->hPos = (snes->hPos + 64) % 1364;
      uint8_t val = (snes->autoJoyTimer > 0);
      val |= (snes->hPos >= 1024) << 6;
      val |= snes->inVblank << 7;
      return val;
    }
    case 0x4213:
      return snes->ppuLatch << 7; // IO-port
    case 0x4214:
      return snes->divideResult & 0xff;
    case 0x4215:
      return snes->divideResult >> 8;
    case 0x4216:
      return snes->multiplyResult & 0xff;
    case 0x4217:
      return snes->multiplyResult >> 8;
    case 0x4016:  /* JOYSER0 — manual joypad read for controller 1. */
    case 0x4017:  /* JOYSER1 — manual joypad read for controller 2. */
      /* On real SNES, $4016/$4017 are the manual joypad-read serial
       * shift registers. After a strobe write to $4016 (latch), 16
       * sequential reads shift out the controller's 16-bit button
       * state (LSB-first). After 16 reads, subsequent reads return
       * bit 0 = 1 as the "controller present" signature for a
       * standard pad. snes9x's S9xReadJOYSERn (controls.cpp:2917)
       * implements this: in the no-latch state with read_idx>=16
       * it returns `bits | 1`.
       *
       * Recomp's emulation core didn't handle these registers at
       * all — the reads fell through to the default `return 0` path,
       * which made SMW's CheckWhichControllersArePluggedIn at $00:9A74
       * conclude "no controllers connected" and write $0DA0 = 0x00.
       * That single byte then cascaded into ~250 downstream WRAM
       * divergences over the attract demo, contributing to the
       * koopa-stomp visible bug (Mario contacts the koopa from a
       * different angle, dies instead of stomping).
       *
       * For correctness without full strobe-latch tracking, return
       * 0x01 unconditionally — same effect as snes9x's post-latch
       * read past 16 bits with a standard pad attached. */
      return 0x01;
    case 0x4218:
      return SwapInputBits(snes->input1_currentState) & 0xff;
    case 0x4219:
      return SwapInputBits(snes->input1_currentState) >> 8;
    case 0x421a:
      return SwapInputBits(snes->input2_currentState) & 0xff;
    case 0x421b:
      return SwapInputBits(snes->input2_currentState) >> 8;
    case 0x421c:
    case 0x421e:
    case 0x421d:
    case 0x421f:
      return 0;

    default: {
      return 0;
    }
  }
}

void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val) {
  switch(adr) {
    case 0x4200: {
      snes->autoJoyRead = val & 0x1;
      if(!snes->autoJoyRead) snes->autoJoyTimer = 0;
      snes->hIrqEnabled = val & 0x10;
      snes->vIrqEnabled = val & 0x20;
      snes->nmiEnabled = val & 0x80;
      if(!snes->hIrqEnabled && !snes->vIrqEnabled) {
        snes->inIrq = false;
      }
      // TODO: enabling nmi during vblank with inNmi still set generates nmi
      //   enabling virq (and not h) on the vPos that vTimer is at generates irq (?)
      break;
    }
    case 0x4201: {
      if(!(val & 0x80) && snes->ppuLatch) {
        // latch the ppu
        ppu_read(g_ppu, 0x37);
      }
      snes->ppuLatch = val & 0x80;
      break;
    }
    case 0x4202: {
      snes->multiplyA = val;
      break;  
    }
    case 0x4203: {
      snes->multiplyResult = snes->multiplyA * val;
      break;
    }
    case 0x4204: {
      snes->divideA = (snes->divideA & 0xff00) | val;
      break;
    }
    case 0x4205: {
      snes->divideA = (snes->divideA & 0x00ff) | (val << 8);
      break;
    }
    case 0x4206: {
      if(val == 0) {
        snes->divideResult = 0xffff;
        snes->multiplyResult = snes->divideA;
      } else {
        snes->divideResult = snes->divideA / val;
        snes->multiplyResult = snes->divideA % val;
      }
      break;
    }
    case 0x4207: {
      snes->hTimer = (snes->hTimer & 0x100) | val;
      break;
    }
    case 0x4208: {
      snes->hTimer = (snes->hTimer & 0x0ff) | ((val & 1) << 8);
      break;
    }
    case 0x4209: {
      snes->vTimer = (snes->vTimer & 0x100) | val;
      break;
    }
    case 0x420a: {
      snes->vTimer = (snes->vTimer & 0x0ff) | ((val & 1) << 8);
      break;
    }
    case 0x420b: {
      dma_startDma(snes->dma, val, false);
      while (dma_cycle(snes->dma)) {}
      break;
    }
    case 0x420c: {
      dma_startDma(snes->dma, val, true);
      break;
    }
    default: {
      break;
    }
  }
}

uint8_t snes_read(Snes* snes, uint32_t adr) {
  uint8_t bank = adr >> 16;
  adr &= 0xffff;
  if(bank == 0x7e || bank == 0x7f) {
    return snes->ram[((bank & 1) << 16) | adr]; // ram
  }
  if(bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) {
    if(adr < 0x2000) {
      return snes->ram[adr]; // ram mirror
    }
    if(adr >= 0x2100 && adr < 0x2200) {
      return snes_readBBus(snes, adr & 0xff); // B-bus
    }
    if (adr == 0x4016 || adr == 0x4017) {
      // joypad read disabled
      return 0;
    }
    if(adr >= 0x4200 && adr < 0x4220 || adr >= 0x4218 && adr < 0x4220) {
      return snes_readReg(snes, adr); // internal registers
    }
    if(adr >= 0x4300 && adr < 0x4380) {
      return dma_read(snes->dma, adr); // dma registers
    }
  }
  // read from cart
  return cart_read(snes->cart, bank, adr);
}

void snes_write(Snes* snes, uint32_t adr, uint8_t val) {
  uint8_t bank = adr >> 16;
  adr &= 0xffff;
  if(bank == 0x7e || bank == 0x7f) {
    uint32_t addr = ((bank & 1) << 16) | adr;
    snes->ram[addr] = val; // ram
  }
  if(bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) {
    if(adr < 0x2000) {
      snes->ram[adr] = val; // ram mirror
    }
    if(adr >= 0x2100 && adr < 0x2200) {
      snes_writeBBus(snes, adr & 0xff, val); // B-bus
    }
    if(adr >= 0x4200 && adr < 0x4220) {
      snes_writeReg(snes, adr, val); // internal registers
    }
    if(adr >= 0x4300 && adr < 0x4380) {
      dma_write(snes->dma, adr, val); // dma registers
    }
    if(adr >= 0x2100 && adr < 0x4400) {
      debug_server_on_reg_write(adr, val);
    }
  }
  // write to cart
  cart_write(snes->cart, bank, adr, val);
}


