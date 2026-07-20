
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
#include "../audio_trace.h"
#include "../cpu_trace.h"
#include "../ppu_dma_trace.h"

int snes_frame_counter;
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

uint8_t snes_readReg(Snes* snes, uint16_t adr);
void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val);

#if SNESRECOMP_TRACE
static void snes_trace_direct_wram_write(uint32_t off, uint8_t old, uint8_t val) {
  extern CpuState g_cpu;
  uint8_t bank = (off >= 0x10000u) ? 0x7f : 0x7e;
  uint16_t addr = (uint16_t)(off & 0xffffu);
  cpu_trace_wram_write_check(&g_cpu, bank, addr, (int32_t)off,
                             (uint16_t)old, (uint16_t)val, 1);
}
#endif

Snes* snes_init(uint8_t *ram) {
  Snes* snes = calloc(1, sizeof(Snes));  /* zero padding: saveload/co-sim hash determinism */
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
  /* Upper cap is a guard against accumulator runaway after a long
   * stall; SPC runs at ~1 MHz so 10000 cycles is about 10 ms of real
   * SPC time per catchup, plenty to absorb any spike. */
  if (snes->apuCatchupCycles > 10000)
    snes->apuCatchupCycles = 10000;

  /* No artificial minimum. Earlier code floored to 1024 SPC cycles
   * per call, which was needed to brute-force progress while the
   * g_apu_autoack stub short-circuited polls. With autoack ripped,
   * the real SPC IPL handshake must run at hardware-realistic
   * timing: ~3.5 SNES-CPU cycles per SPC cycle, which works out to
   * ~73 SPC cycles per HW-reg touch (cpu_pace_cycles bumps 256 main
   * cycles per touch -> 256 * 2/7 is about 73). Flooring to 1024 made each
   * SMW upload byte take ~3000 SPC cycles instead of ~219, blowing
   * past the 5-second per-frame watchdog before the ~10 KB SPC
   * engine could finish uploading. The audio thread separately
   * cycles the SPC in bulk via RtlRenderAudio (534 samples is about 17 k
   * cycles per audio callback), so the SPC always gets enough
   * time even when the CPU is busy elsewhere. */
  int catchupCycles = (int) snes->apuCatchupCycles;
  if (catchupCycles < 0) catchupCycles = 0;

  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  for(int i = 0; i < catchupCycles; i++) {
    apu_cycle(snes->apu);
  }
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
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
    // APU port read ($2140-$217F). Synchronize the SPC to this exact guest
    // timestamp before observing its output-port state.
    RtlApuLock();
    rtl_sync_apu_to_cpu_locked();
    uint8_t v = snes->apu->outPorts[adr & 0x3];
    audio_trace_on_cpu_port_read((uint8_t)(adr & 0x3), v);
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
      uint32_t wa = snes->ramAdr & 0x1ffffu;
      uint8_t old = snes->ram[wa];
      snes->ram[wa] = val;
#if SNESRECOMP_TRACE
      snes_trace_direct_wram_write(wa, old, val);
#endif
#if SNESRECOMP_REVERSE_DEBUG
      { extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
        debug_on_wram_write_byte(wa, old, val); }
#endif
      snes->ramAdr = (wa + 1u) & 0x1ffffu;
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

static void snes_advance_beam(Snes *snes, uint32_t clocks, bool check_irq) {
  /* One NTSC scanline is 1364 master clocks, 262 scanlines per frame. Keep
   * the live beam counters moving while LLE code executes so SLHV/OPHCT/OPVCT
   * polling observes hardware time rather than a frame-frozen PPU.
   *
   * The CPU IRQ comparators run for the entire scanline/frame, including
   * vblank.  Keeping this here (instead of in the visible-line renderer) is
   * important for games such as Star Fox, which deliberately schedules an
   * H+V IRQ on scanline 228 and blocks until that IRQ completes an NMI task. */
  uint32_t h = snes->hPos;
  uint32_t v = snes->vPos;
  while (clocks) {
    uint32_t span = 1364u - h;
    if (span > clocks) span = clocks;

    /* Automatic joypad polling begins at vblank and keeps HVBJOY.0 asserted
     * for roughly 4224 master clocks.  The input registers are already backed
     * by the current controller snapshot; this timer supplies the missing
     * architectural start/busy/complete handshake. */
    if (snes->autoJoyRead && v == 225u && h == 0u)
      snes->autoJoyTimer = 4224;
    if (snes->autoJoyTimer) {
      snes->autoJoyTimer = span >= snes->autoJoyTimer
                         ? 0 : (uint16_t)(snes->autoJoyTimer - span);
    }

    if (check_irq && (snes->hIrqEnabled || snes->vIrqEnabled)) {
      bool line_matches = !snes->vIrqEnabled || v == snes->vTimer;
      uint32_t target = snes->hIrqEnabled ? (uint32_t)snes->hTimer * 4u : 0u;
      if (line_matches && target < 1364u && target >= h && target < h + span)
        snes->inIrq = true;
    }

    h += span;
    clocks -= span;
    if (h >= 1364u) {
      h = 0;
      v++;
      if (v >= 262u) v = 0;
    }
  }
  snes->hPos = (uint16_t)h;
  snes->vPos = (uint16_t)v;
  snes->inVblank = v >= 225u;
}

void snes_advance_master_cycles(Snes *snes, uint32_t clocks) {
  snes_advance_beam(snes, clocks, true);
  snes->beamMasterLast += clocks;
}

void snes_sync_master_clock(Snes *snes, uint64_t master_clock) {
  if (master_clock < snes->beamMasterLast) {
    snes->beamMasterLast = master_clock;
    return;
  }
  uint64_t delta=master_clock-snes->beamMasterLast;
  while(delta) {
    uint32_t chunk=delta>UINT32_MAX?UINT32_MAX:(uint32_t)delta;
    snes_advance_master_cycles(snes,chunk);
    delta-=chunk;
  }
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
      // Static-recomp h/v-counter model: real hardware updates hPos every
      // dot-clock; recomp has no dot-clock, so each $4212 read advances
      // hPos by a fixed step. Calibrated so a typical busy-wait crosses
      // both edges in ~10-20 reads. Bit 6 = hblank (dots ~1024..1364 of
      // a 1364-dot scanline). See docs/VIRTUAL_HW_CONTRACT.md.
      snes_advance_beam(snes, 64, false);
      // Bit 7 = vblank. The real frame loop drives vblank via inNmi, not
      // inVblank (inVblank is never set true), so on the static-recomp
      // path bit 7 must be SYNTHESIZED the same way bit 6 is — otherwise a
      // boot vblank-wait loop that polls $4212 bit 7 BEFORE the first NMI
      // (while the single host fiber is blocked in SwitchToFiber and real
      // frame timing is frozen) never sees the edge and spins forever
      // (Super Metroid I_RESET $00:843C: `LDA $4212 / BPL` x4 settle).
      // The synthetic advance shares the live beam helper, so IRQ and
      // auto-joypad edges remain coherent with the reported counters.
      uint8_t val = (snes->autoJoyTimer > 0);
      val |= (snes->hPos >= 1024) << 6;
      val |= (snes->inVblank || snes->vPos >= 225) << 7;
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
      /* Always-on observability: record each triggered channel's config
       * before the transfer consumes aAdr/size (see ppu_dma_trace.h). */
      for (int ch = 0; ch < 8; ch++) {
        if (val & (1 << ch)) {
          DmaChannel *c = &snes->dma->channel[ch];
          ppudma_record_dma(ch, c->fromB, c->aBank, c->aAdr, c->bAdr, c->size);
        }
      }
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
    uint8_t old = snes->ram[addr];
    snes->ram[addr] = val; // ram
#if SNESRECOMP_TRACE
    snes_trace_direct_wram_write(addr, old, val);
#endif
#if SNESRECOMP_REVERSE_DEBUG
    { extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
      debug_on_wram_write_byte(addr, old, val); }
#endif
  }
  if(bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) {
    if(adr < 0x2000) {
      uint8_t old = snes->ram[adr];
      snes->ram[adr] = val; // ram mirror
#if SNESRECOMP_TRACE
      snes_trace_direct_wram_write((uint32_t)adr, old, val);
#endif
#if SNESRECOMP_REVERSE_DEBUG
      { extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
        debug_on_wram_write_byte((uint32_t)adr, old, val); }
#endif
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
