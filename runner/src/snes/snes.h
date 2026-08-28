
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
  bool joypadStrobe;
  uint8_t joypad1Index;
  uint8_t joypad2Index;
  uint16_t joypad1Latched;
  uint16_t joypad2Latched;
  bool disableRender;

  // ram data port ($2180-$2183)
  uint32_t ramAdr;
  uint8_t *ram;

  // Host timing anchor; excluded from savestates and reconciled after load.
  uint64_t beamMasterLast;

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

  /* IRQ comparator instrumentation. A frame-model host cannot measure these
   * from outside: polling the debug server tops out around 20 samples/second
   * against a 60 Hz field, so a match that is swept over is invisible to any
   * external probe. Counted here, they are exact.
   *   dbgIrqLatches   comparator matches that asserted the IRQ line
   *   dbgIrqMissed    FIELDS that ended with the comparator armed and nothing
   *                   latched — i.e. the beam stepped OVER the target, which
   *                   the window test (target inside [h, h+span)) turns into
   *                   no interrupt at all rather than a late one
   *   dbgLastMissedLine  the vTimer value in force when that last happened */
  uint32_t dbgIrqLatches;
  uint32_t dbgIrqMissed;
  uint16_t dbgLastMissedLine;
  uint8_t  dbgLatchedThisField;
  /* Overshoot: arrived on the target line already past the target column, so
   * the match cannot fire this field. The one number that separates "the beam
   * stepped over the interrupt" from "the game never asked for one". */
  uint32_t dbgIrqOvershot;
  uint32_t dbgMissDedupe;
  uint16_t dbgLastOvershotLine;
  uint16_t dbgLastOvershotBy;
  /* Targets programmed behind the beam — see the comment at the $4209 write. */
  uint32_t dbgTargetInPast;
  uint16_t dbgLastPastTarget;
  uint16_t dbgLastPastBeam;
  /* Where the beam actually was when the host delivered NMI, and how far
   * behind the CPU the beam had fallen at that moment. On hardware NMI is a
   * vblank event, so the beam is at ~line 225-262; a value inside the visible
   * field means the host's frame boundary and the PPU's field boundary have
   * drifted apart, and every split the NMI schedules for an early line is
   * already in the past. */
  uint16_t dbgNmiBeamLine;
  uint32_t dbgNmiLateCount;
  uint64_t dbgBeamLagAtNmi;
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
/* Savestate format version for snes_saveload layout (RTLS header). */
void snes_saveload_set_version(uint32_t version);
void snes_saveload(Snes *snes, SaveLoadInfo *sli);
void snes_catchupApu(Snes *snes);
void snes_advance_master_cycles(Snes *snes, uint32_t clocks);
void snes_sync_master_clock(Snes *snes, uint64_t master_clock);
/* Master clock at which the enabled H/V IRQ comparator next matches, starting
 * from the live beam position (`now` is that position's master clock, normally
 * g_cpu.master_cycles). Returns false when no comparator is armed.
 *
 * `snes->inIrq` is a single latch, so a host that lets the CPU run a whole
 * frame per slice coalesces every raster IRQ in that frame into one delivery.
 * That silently breaks chained raster splits - a handler that re-arms vTimer
 * for the next band never sees its band, because the beam is already past it
 * when the host next looks. (F-Zero chains four splits per frame at lines
 * 18/28/47/86; only line 18 survived.) A frame-model host should cap each
 * execution slice at this clock so every comparator edge is delivered at its
 * own beam position. See docs/FRAME_MODEL_HOSTS.md. */
bool snes_next_irq_master(const Snes *snes, uint64_t now, uint64_t *out);

/* Master clocks from the current beam position to the next programmed H/V IRQ
 * match, or 0 when none is scheduled (or one is already pending).
 *
 * For a frame-model host this is a SCHEDULER bound, not a step size: run the
 * CPU only up to the next event, so it cannot execute past a raster split and
 * make the IRQ arrive late. snes_advance_beam's comparator is a window test
 * (target inside [h, h+span)), so an overshoot yields no IRQ at all rather
 * than a late one — and a split scheduled two scanlines after the one that
 * scheduled it, as Gundam Wing does at $00:888E, is lost outright. */
uint32_t snes_master_clocks_until_irq(const Snes *snes);

/* Clocks to the start of scanline `line`, for a host that needs to place its
 * frame boundary on the PPU's field boundary. See the implementation. */
uint32_t snes_master_clocks_until_line(const Snes *snes, uint32_t line);

/* Freeze (1) / release (0) the beam across an IRQ handler dispatch — see the
 * comment on the implementation. The host's frame loop brackets
 * interp_bridge_run_interrupt with this for raster IRQs. */
void snes_beam_hold(int hold);

extern int snes_frame_counter;
#endif
