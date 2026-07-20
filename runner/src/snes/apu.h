#ifndef APU_H
#define APU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Apu Apu;

#include "snes.h"
#include "spc.h"
#include "dsp.h"

typedef struct Timer {
  uint8_t cycles;
  uint8_t divider;
  uint8_t target;
  uint8_t counter;
  bool enabled;
} Timer;

/* CPU->APU bus events are timestamped in guest APU cycles. The audio callback
 * may execute the SPC ahead of or behind the CPU thread, but it must not turn
 * two writes from different emulated frames into back-to-back host mutations.
 * A power-of-two queue preserves global CPU bus order. If it ever fills, the
 * caller advances the SPC until space exists; no event is overwritten or
 * force-applied before its guest timestamp. */
#define APU_PORT_QUEUE_LEN 1024u

typedef struct ApuPortWrite {
  uint64_t target_cycle; /* apply when portClock reaches this APU cycle */
  uint8_t port;          /* 0-3 */
  uint8_t val;
} ApuPortWrite;

struct Apu {
  Spc* spc;
  Dsp* dsp;
  uint8_t ram[0x10000];
  bool romReadable;
  uint8_t dspAdr;
  uint32_t cycles;
  uint8_t inPorts[6]; // includes 2 bytes of ram
  uint8_t outPorts[4];
  Timer timer[3];
  uint8_t cpuCyclesLeft;
  uint8_t pad[6];
  /* Port event state must stay after pad: apu_saveload snapshots [ram,pad+6),
   * and that savestate layout is frozen. Callback lead is host state, so the
   * queue and its mapping are deliberately reset rather than serialized. */
  ApuPortWrite portQueue[APU_PORT_QUEUE_LEN];
  uint32_t portQHead;
  uint32_t portQTail;
  uint64_t portClock;        /* APU cycles executed since reset */
  uint64_t portGuestAnchor;  /* guest-cycle origin for current mapping */
  uint64_t portTargetAnchor; /* matching portClock origin */
  uint64_t portLastGuest;
  uint64_t portLastTarget;
  bool portTimeValid;
};

Apu* apu_init();
void apu_free(Apu* apu);
void apu_reset(Apu* apu);
void apu_cycle(Apu* apu);
uint8_t apu_cpuRead(Apu* apu, uint16_t adr);
void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val);
void apu_saveload(Apu *apu, SaveLoadInfo *sli);

/* Immediate visibility is reserved for boot and synchronous protocol code
 * which advances the SPC itself. Caller holds RtlApuLock. */
void apu_writePortNow(Apu* apu, uint8_t port, uint8_t val);
/* Map a monotonic guest APU-cycle timestamp onto the live SPC timeline and
 * enqueue the write. False means the caller must advance the APU and retry. */
bool apu_schedulePortWrite(Apu* apu, uint8_t port, uint8_t val,
                           uint64_t guest_cycle);
uint32_t apu_portQueueDepth(const Apu* apu);
/* Advance real SPC execution until every queued bus event has landed. */
bool apu_runUntilPortQueueEmpty(Apu* apu, uint32_t max_cycles);
/* Advance the live SPC to a guest timestamp using the same mapping as queued
 * CPU writes. This is the fast-forward coupling point: game frames and SPC
 * state advance together even when the host audio device cannot play them. */
bool apu_runToGuestCycle(Apu* apu, uint64_t guest_cycle,
                         uint32_t max_cycles);
/* HLE for a declared live transfer protocol: wait for the driver-ready AA/BB
 * pair, then deliver the standard CC terminator and wait for its echo. */
bool apu_waitForTransferReady(Apu* apu, uint8_t request_port,
                              uint8_t request_value, uint32_t max_cycles);
bool apu_finishHleTransfer(Apu* apu, uint16_t final_pc,
                           uint32_t max_cycles);
/* Drop pending events and reset the guest-to-SPC mapping. */
void apu_clearPortQueue(Apu* apu);

#endif
