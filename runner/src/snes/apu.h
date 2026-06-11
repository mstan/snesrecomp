
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

/* CPU->APU port write, scheduled in APU-sample time.
 *
 * Why a queue: the audio thread advances the SPC in whole-callback
 * bursts (~534 samples at a time) while the CPU writes ports at wall
 * time. Mutating inPorts at wall time compresses each value's lifetime
 * to however many samples happen to be produced between two writes —
 * measured at ~9 samples for SMW's one-frame sound commands whenever
 * the NMI (60.0988 Hz) and audio-callback (60.00 Hz) phases cross,
 * which is less than one engine poll period (64 samples): the engine
 * provably never sees the command and the sound is silently dropped,
 * in beating ~10 s runs. Scheduling each write at a fixed horizon on
 * the consumption clock restores the hardware-faithful timeline: in
 * the SPC's own execution time, consecutive frame writes stay a full
 * frame (~534 samples) apart regardless of burst alignment. */
#define APU_PORT_QUEUE_LEN 64u  /* power of 2; SMW peaks at 4 writes/frame */
typedef struct ApuPortWrite {
  uint64_t target_sample; /* apply when the produced-sample clock reaches this */
  uint8_t port;           /* 0-3 */
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
  /* Port-write scheduler — MUST stay after `pad`: apu_saveload snapshots
   * [ram, pad+6) and the savestate layout is frozen. Cleared on reset
   * and on the HLE SPC-image upload; deliberately not serialized (any
   * still-pending write applies on the first cycles after load). */
  ApuPortWrite portQueue[APU_PORT_QUEUE_LEN];
  uint32_t portQHead;     /* next slot to apply  */
  uint32_t portQTail;     /* next slot to fill   */
};

Apu* apu_init();
void apu_free(Apu* apu);
void apu_reset(Apu* apu);
void apu_cycle(Apu* apu);
uint8_t apu_cpuRead(Apu* apu, uint16_t adr);
void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val);
void apu_saveload(Apu *apu, SaveLoadInfo *sli);
/* Schedule a CPU-side port write ($2140+port) to land in inPorts when
 * the produced-sample clock reaches target_sample. Caller must hold
 * RtlApuLock. Queue overflow applies the oldest entry immediately
 * (order is always preserved). */
void apu_schedulePortWrite(Apu* apu, uint8_t port, uint8_t val,
                           uint64_t target_sample);
/* Drop all pending scheduled port writes (reset / HLE image upload). */
void apu_clearPortQueue(Apu* apu);
#endif
