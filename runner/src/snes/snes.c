
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
#include "joypad.h"
#include "variables.h"
#include "../common_rtl.h"
#include "../cpu_state.h"
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

static SnesMasterClockChargeHook s_master_clock_charge_hook;
static SnesWramWriteLogHook s_wram_write_log_hook;

void snes_set_master_clock_charge_hook(SnesMasterClockChargeHook hook) {
  s_master_clock_charge_hook = hook;
}

void snes_set_wram_write_log_hook(SnesWramWriteLogHook hook) {
  s_wram_write_log_hook = hook;
}

static void snes_charge_master_cycles(Snes *snes, uint64_t clocks) {
  if (s_master_clock_charge_hook) {
    s_master_clock_charge_hook(snes, clocks);
    return;
  }
  while (clocks) {
    uint32_t chunk = clocks > 0xffffffffull ? 0xffffffffu : (uint32_t)clocks;
    snes_advance_master_cycles(snes, chunk);
    clocks -= chunk;
  }
}

static void snes_note_direct_wram_write(uint32_t ram_off, uint8_t value,
                                        const char *via) {
  if (s_wram_write_log_hook)
    s_wram_write_log_hook(ram_off, value, via);
}

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

/* RTLS v5 and earlier serialized beamMasterLast between vPos and
 * apuCatchupCycles (+8 bytes). v6+ keeps it host-only (before hPos). */
static uint32_t s_saveload_version = 7;

void snes_saveload_set_version(uint32_t version) {
  s_saveload_version = version ? version : 7;
}

void snes_saveload(Snes *snes, SaveLoadInfo *sli) {
  cpu_saveload(snes->cpu, sli);
  apu_saveload(snes->apu, sli);
  dma_saveload(snes->dma, sli);
  ppu_saveload(snes->ppu, sli);
  cart_saveload(snes->cart, sli);

  if (s_saveload_version <= 5) {
    /* Format 4/5 can no longer be mapped onto this struct, so refuse it
     * rather than mis-load it.
     *
     * The old layout was [hPos..vPos][pad][beamMasterLast][apuCatchup..
     * divideResult], and the code here copied that trailing region as one
     * 32-byte block on the assumption that it was still contiguous and still
     * 32 bytes. Both stopped being true: the dbgIrq / dbgNmi counters were
     * later inserted between the interrupt fields and inVblank, so
     * the region now measures 88 bytes AND has different contents in the
     * middle. The copy therefore overran a 48-byte stack buffer by 56 bytes
     * (caught by -Wfortify-source), and had it been merely clamped it would
     * have loaded the wrong bytes into hTimer/vTimer and the IRQ flags --
     * a silent corruption, which is worse than a refusal.
     *
     * Reconstructing a correct v4/v5 mapping is possible but untestable here
     * with no such file to verify against, so this states the limit instead
     * of guessing at it. RTL_SAV_VERSION_MIN is raised to 6 to match, which
     * means this branch is unreachable through RtlLoadSnapshot; it stays as
     * the explanation for anyone who tries to lower that bound again. */
    assert(!"savestate format 4/5 is no longer supported");
  } else {
    /* The savestate format IS this byte range. Adding or reordering any field
     * after hPos silently changes it and quietly invalidates every existing
     * save, so pin the size: if this fires, bump RTL_SAV_VERSION rather than
     * deleting the assertion. (The blob is raw struct memory, so it was never
     * portable across differing layouts; pinning it here at least makes a
     * change visible at compile time.) */
    _Static_assert(sizeof(Snes) - offsetof(Snes, hPos) == 96,
                   "Snes savestate tail changed size -- bump RTL_SAV_VERSION");
    sli->func(sli, &snes->hPos, sizeof(*snes) - offsetof(Snes, hPos));
  }
  sli->func(sli, snes->ram, 0x20000);
  sli->func(sli, &snes->ramAdr, 4);
  if (s_saveload_version >= 7) {
    sli->func(sli, &snes->joypadStrobe, sizeof(snes->joypadStrobe));
    sli->func(sli, &snes->joypad1Index, sizeof(snes->joypad1Index));
    sli->func(sli, &snes->joypad2Index, sizeof(snes->joypad2Index));
    sli->func(sli, &snes->joypad1Latched, sizeof(snes->joypad1Latched));
    sli->func(sli, &snes->joypad2Latched, sizeof(snes->joypad2Latched));
  } else {
    snes->joypadStrobe = false;
    snes->joypad1Index = snes->joypad2Index = 0;
    snes->joypad1Latched = snes->joypad2Latched = 0;
  }

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
  snes->joypadStrobe = false;
  snes->joypad1Index = snes->joypad2Index = 0;
  snes->joypad1Latched = snes->joypad2Latched = 0;
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
   * SPC time per catchup, plenty to absorb any spike. This is a frequent
   * catch-up API, not a frame-at-a-time pacing API; hosts that own their
   * own frame model should catch up at smaller slices or use the guest-time
   * frame runner path. See docs/FRAME_MODEL_HOSTS.md. */
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
      snes_note_direct_wram_write(wa, val, "wmdata");
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
  return joypad_auto_read_word(x);
}

/* Returns the master clocks actually consumed. Normally == clocks; less when
 * the walk LATCHES an IRQ, where it stops one clock past the comparator match
 * and leaves the remainder pending. An IRQ pre-empts: beam time between the
 * latch and the handler belongs to code the real CPU would only execute after
 * the handler returned, so walking it before the handler runs makes any split
 * the handler schedules close ahead land in the past (Gundam Wing schedules
 * VTIME=+2 lines from its line-21 handler; that split was lost ~85% of frames
 * and the gameplay demo rendered black at brightness 0). While inIrq is
 * pending the walk consumes nothing; the host frame loop dispatches promptly,
 * the handler runs under snes_beam_hold, and the walk resumes on release. */
static uint32_t snes_advance_beam(Snes *snes, uint32_t clocks, bool check_irq) {
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
  uint32_t consumed = 0;
  if (check_irq && snes->inIrq)
    return 0;                      /* frozen until the pending IRQ is taken */
  while (clocks) {
    uint32_t span = 1364u - h;
    if (check_irq && v < 225u && h < 1024u && span > 1024u - h)
      span = 1024u - h;
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
      /* Overshoot detection, exact. We are ON the target line but the beam is
       * already PAST the target column, so the window test below cannot fire
       * and the walk will leave this line without an interrupt. That is a lost
       * match, and it is invisible to any external probe: the comparator stays
       * armed and the beam keeps moving, which looks identical to a scene that
       * simply has no split there.
       *
       * Deduplicated per (field, target): the span loop can visit one line in
       * several iterations, and a repeat is the same miss, not a new one. */
      if (line_matches && target < 1364u && target < h &&
          !snes->dbgLatchedThisField &&
          snes->dbgMissDedupe != (uint32_t)((v << 16) | target)) {
        snes->dbgMissDedupe = (uint32_t)((v << 16) | target);
        snes->dbgIrqOvershot++;
        snes->dbgLastOvershotLine = (uint16_t)v;
        snes->dbgLastOvershotBy = (uint16_t)(h - target);
      }
      if (line_matches && target < 1364u && target >= h && target < h + span) {
        uint32_t upto = target - h + 1u;   /* stop one clock past the match */
        snes->inIrq = true;
        snes->dbgIrqLatches++;
        snes->dbgLatchedThisField = 1;
        h += upto;
        consumed += upto;
        if (h >= 1364u) { h = 0; v++; if (v >= 262u) v = 0; }
        snes->hPos = (uint16_t)h;
        snes->vPos = (uint16_t)v;
        snes->inVblank = v >= 225u;
        return consumed;
      }
    }

    h += span;
    clocks -= span;
    consumed += span;
    if (check_irq && v < 225u && h == 1024u)
      dma_doHdma(snes->dma);
    if (h >= 1364u) {
      h = 0;
      v++;
      if (v >= 262u) {
        v = 0;
        if (check_irq)
          dma_initHdma(snes->dma);
        /* End of field. Armed the whole way round and nothing latched means
         * the beam swept past the target without firing — a LOST interrupt,
         * not a pending one. */
        if (check_irq && (snes->hIrqEnabled || snes->vIrqEnabled) &&
            !snes->dbgLatchedThisField) {
          snes->dbgIrqMissed++;
          snes->dbgLastMissedLine = snes->vTimer;
        }
        snes->dbgLatchedThisField = 0;
      }
    }
  }
  snes->hPos = (uint16_t)h;
  snes->vPos = (uint16_t)v;
  snes->inVblank = v >= 225u;
  return consumed;
}

void snes_advance_master_cycles(Snes *snes, uint32_t clocks) {
  uint32_t consumed = snes_advance_beam(snes, clocks, true);
  snes->beamMasterLast += consumed;
}

/* Master clocks from the beam's current position to the start of scanline
 * `line` in the CURRENT or next field. Zero when already exactly there.
 *
 * A frame-model host needs this to put its frame boundary on the PPU's field
 * boundary. Without it the host's "one frame" is a fixed 357368-clock budget
 * measured from wherever the previous frame happened to stop, so the phase
 * between the host frame and the PPU field is arbitrary and drifts. NMI then
 * arrives at a random scanline instead of at vblank, and any split the NMI
 * schedules for an early line can already be behind the beam — which is a
 * silently lost interrupt, because the comparator is a window test. Measured
 * on Gundam Wing: NMI arrived at line 18 on the first attract loop and lines
 * 30/34 on later ones, and from the moment it passed line 21 the raster chain
 * lost its first link on every single frame. */
uint32_t snes_master_clocks_until_line(const Snes *snes, uint32_t line) {
  uint32_t h, v, lines;
  if (!snes || line >= 262u) return 0;
  h = snes->hPos;
  v = snes->vPos;
  if (v == line && h == 0u) return 0;
  lines = (line > v) ? (line - v) : (262u - v + line);
  return lines * 1364u - h;
}

uint32_t snes_master_clocks_until_irq(const Snes *snes) {
  if (!snes) return 0;
  if (snes->inIrq) return 0;                       /* already pending */
  if (!snes->hIrqEnabled && !snes->vIrqEnabled) return 0;

  uint32_t h = snes->hPos;
  uint32_t v = snes->vPos;
  uint32_t target_h = snes->hIrqEnabled ? (uint32_t)snes->hTimer * 4u : 0u;
  if (target_h >= 1364u) return 0;                 /* unreachable H target */

  if (!snes->vIrqEnabled) {
    /* H-only: fires every line — later this line, or on the next. */
    return target_h > h ? (target_h - h) : (1364u - h + target_h);
  }

  {
    /* V (with or without H): one match per field, at (vTimer, target_h). */
    uint32_t target_v = snes->vTimer;
    uint32_t lines;
    uint64_t d;
    if (target_v >= 262u) return 0;
    if (target_v > v)      lines = target_v - v;
    else if (target_v < v) lines = 262u - v + target_v;
    else                   lines = (target_h > h) ? 0u : 262u;
    /* lines==0 implies target_h>h, so this stays positive; every other branch
       adds at least a whole line, so the subtraction cannot underflow. */
    d = (uint64_t)lines * 1364u + (uint64_t)target_h - (uint64_t)h;
    return d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d;
  }
}

/* ── beam hold ─────────────────────────────────────────────────────────────
 * While set, snes_sync_master_clock is a no-op: the beam stays where it is and
 * the CPU-time delta simply accumulates, to be walked on the first sync after
 * release.
 *
 * Why this exists: a raster-IRQ handler on silicon runs within ~tens of master
 * clocks of the latch — the beam barely moves before the handler's register
 * writes land. This runtime cannot pre-empt AOT-compiled code mid-function, so
 * by the time a handler is dispatched, master_cycles already carries scanlines
 * of mainline execution the real CPU would have performed AFTER the handler.
 * Letting the beam walk through that inflated delta DURING the handler makes
 * the handler appear scanlines long, and any split it schedules close ahead
 * (Gundam Wing: VTIME=+2 lines from the line-21 handler) lands in the past and
 * never fires. Holding the beam for the handler's duration models the truth —
 * handlers are short — rather than the artefact.
 *
 * While held, beam-position reads ($2137/$4212) return the latch-point line;
 * that is exactly what a real handler racing its own splits would observe. */
static int s_beam_hold;

void snes_beam_hold(int hold) { s_beam_hold = hold; }

void snes_sync_master_clock(Snes *snes, uint64_t master_clock) {
  if (s_beam_hold) return;
  if (master_clock < snes->beamMasterLast) {
    snes->beamMasterLast = master_clock;
    return;
  }
  uint64_t delta=master_clock-snes->beamMasterLast;
  while(delta) {
    uint32_t chunk=delta>UINT32_MAX?UINT32_MAX:(uint32_t)delta;
    uint64_t before=snes->beamMasterLast;
    snes_advance_master_cycles(snes,chunk);
    /* A short walk means an IRQ latched (or is still pending): the beam
     * pre-empts there. The remaining delta stays owed and is walked on the
     * first sync after the handler has been taken. */
    if (snes->beamMasterLast - before < chunk)
      break;
    delta-=chunk;
  }
}

bool snes_next_irq_master(const Snes *snes, uint64_t now, uint64_t *out) {
  if (!snes || !out) return false;
  if (!snes->hIrqEnabled && !snes->vIrqEnabled) return false;
  /* Mirrors the match test in snes_advance_beam(): the comparator fires when
   * the beam crosses target_h on a line the V comparator accepts. */
  const uint32_t target_h =
      snes->hIrqEnabled ? (uint32_t)snes->hTimer * 4u : 0u;
  if (target_h >= 1364u) return false;

  uint32_t h = snes->hPos;
  uint32_t v = snes->vPos;
  uint64_t delta = 0;
  for (uint32_t scanned = 0; scanned <= 262u; scanned++) {
    const bool line_matches = !snes->vIrqEnabled || v == snes->vTimer;
    if (line_matches && target_h >= h) {
      *out = now + delta + (uint64_t)(target_h - h);
      return true;
    }
    delta += 1364u - h;
    h = 0;
    v = (v + 1u) % 262u;
  }
  return false;
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
      /* Whole-program interpreter runs already advance the beam from every
       * opcode's measured master clocks. Applying the legacy static-recomp
       * polling tick as well doubles time in $4212 wait loops (SMRPG's boot
       * fades completed in half the reference frame count). */
      extern int g_interp_apu_driving;
      if (!g_interp_apu_driving)
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
      /* $4016 bit 0 latches both pads; reads shift B through R, then 1s. */
      return joypad_read_serial(snes, adr - 0x4016);
    case 0x4218:
      return joypad_auto_read_reg(snes->input1_currentState, adr);
    case 0x4219:
      return joypad_auto_read_reg(snes->input1_currentState, adr);
    case 0x421a:
      return joypad_auto_read_reg(snes->input2_currentState, adr);
    case 0x421b:
      return joypad_auto_read_reg(snes->input2_currentState, adr);
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
    case 0x4016:
      joypad_write_strobe(snes, val);
      break;
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
      /* Was this target programmed into the PAST? A raster chain schedules its
       * next split from inside the current one, so the new target is normally
       * a few lines ahead of the beam. If it is already behind, the comparator
       * cannot match until the counter wraps — and by then the NMI has reset
       * the chain, so the split is lost for good.
       *
       * This is the one failure an external probe cannot see: the comparator
       * stays armed, the beam keeps sweeping, nothing is "missed" on the target
       * line because the beam is never on it. It looks exactly like a scene
       * that simply has no split there. */
      /* Only while the beam is in the VISIBLE field. A target programmed
       * during vblank is for the next field and is legitimately "behind" the
       * current beam position — counting it would flag correct scheduling as a
       * fault, which is how an instrument invents a regression. */
      if (snes->vIrqEnabled && snes->vPos < 225u &&
          snes->vTimer < snes->vPos) {
        snes->dbgTargetInPast++;
        snes->dbgLastPastTarget = snes->vTimer;
        snes->dbgLastPastBeam = snes->vPos;
      }
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
      /* Charge the transfer's guest time BEFORE running it. On hardware a
       * general-purpose DMA halts the CPU for ~8 master clocks per byte plus
       * small per-channel setup; this loop used to complete the whole
       * transfer in ZERO guest time. That is not a rounding error: a scene
       * load moves enough data that the free time let the loader finish its
       * lag blocks measurably earlier than hardware (33/46/32 frames vs
       * Mesen's 36/47/35 for the identical flow), which shifted the parity
       * of the pass that spawns the mechs — and the sprite hover (gated on
       * the pass counter) then paired with the wrong animation phase,
       * publishing Y±1 sprite tables hardware never shows. See
       * gundamwing-parity-root-cause. HDMA stays uncharged here (per-line,
       * far smaller; out of scope for this fix). */
      {
        uint64_t dma_master = 12;
        for (int ch = 0; ch < 8; ch++) {
          if (val & (1 << ch)) {
            uint32_t n = snes->dma->channel[ch].size;
            if (n == 0) n = 0x10000;
            dma_master += 8 + (uint64_t)n * 8;
          }
        }
        snes_charge_master_cycles(snes, dma_master);
      }
      dma_startDma(snes->dma, val, false);
      while (dma_cycle(snes->dma)) {}
      break;
    }
    case 0x420c: {
      /* LLE (and AOT via recomp_write_internal_reg) both land here. Presentation
       * draws (SimpleHdma) re-arm from this latch — without it, LLE games keep
       * last_hdmaen at 0 and wipe channel hdmaActive before every present. */
      g_snesrecomp_last_hdmaen = val;
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
    if (adr == 0x4016 || adr == 0x4017)
      return snes_readReg(snes, adr);
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
    snes_note_direct_wram_write(addr, val, "snes_write");
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
      snes_note_direct_wram_write((uint32_t)adr, val, "snes_write_mirror");
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
    if(adr == 0x4016) {
      snes_writeReg(snes, adr, val); // manual joypad strobe
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
