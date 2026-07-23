
#ifndef DSP_H
#define DSP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "saveload.h"

typedef struct Dsp Dsp;

typedef struct Apu Apu;

// Output-sample ring capacity (stereo pairs). Must be a power of two so
// the monotonic write/read counters can index with a mask and survive
// uint32 wraparound. 8192 samples ≈ 256 ms at 32 kHz — far larger than
// any single-frame APU catch-up burst, while typical fill stays ~534
// (one block), so playback latency is unchanged. See the sampleBuffer
// comment in struct Dsp and the music-tick post-mortem in MMX ISSUES.md.
#define DSP_SAMPLE_RING 8192

typedef struct DspChannel {
  // pitch
  uint16_t pitch;
  uint16_t pitchCounter;
  bool pitchModulation;
  // brr decoding
  int16_t decodeBuffer[19]; // 16 samples per brr-block, +3 for interpolation
  uint8_t srcn;
  uint16_t decodeOffset;
  uint8_t previousFlags; // from last sample
  int16_t old;
  int16_t older;
  bool useNoise;
  // adsr, envelope, gain
  uint16_t adsrRates[4]; // attack, decay, sustain, gain
  uint16_t rateCounter;
  uint8_t adsrState; // 0: attack, 1: decay, 2: sustain, 3: gain, 4: release
  uint16_t sustainLevel;
  bool useGain;
  uint8_t gainMode;
  bool directGain;
  uint16_t gainValue; // for direct gain
  uint16_t gain;
  // keyon/off
  bool keyOn;
  bool keyOff;
  // output
  int16_t sampleOut; // final sample, to be multiplied by channel volume
  int8_t volumeL;
  int8_t volumeR;
  bool echoEnable;
} DspChannel;

struct Dsp {
  uint8_t *apu_ram;
  // MP2K-style verified-enhancement shadow mixer (opt-in; default off).
  // Placed BEFORE `ram` so it lies outside the dsp_saveload region
  // (which serializes from `ram` to end) — savestate layout is unchanged.
  // void* to keep dsp.h free of the shadow header; dsp.c owns the type.
  void *shadow;
  // mirror ram
  uint8_t ram[0x80];
  // 8 channels
  DspChannel channel[8];
  // overarching
  uint16_t dirPage;
  bool evenCycle;
  bool mute;
  bool reset;
  int8_t masterVolumeL;
  int8_t masterVolumeR;
  // noise
  int16_t noiseSample;
  uint16_t noiseRate;
  uint16_t noiseCounter;
  // echo
  bool echoWrites;
  int8_t echoVolumeL;
  int8_t echoVolumeR;
  int8_t feedbackVolume;
  uint16_t echoBufferAdr;
  uint16_t echoDelay;
  uint16_t echoRemain;
  uint16_t echoBufferIndex;
  uint8_t firBufferIndex;
  int8_t firValues[8];
  int16_t firBufferL[8];
  int16_t firBufferR[8];
  // Output-sample ring. Two producers feed it (serialized by RtlApuLock):
  // the audio thread (RtlRenderAudio) and the CPU thread (snes_catchupApu,
  // on APU-port access). The old fixed 534-sample buffer dropped every
  // sample produced past 534, so a catch-up burst between audio callbacks
  // lost samples → music-rate ticks + timing jitter. The ring buffers the
  // burst instead; the audio thread consumes the oldest 534 per block at
  // the steady output rate, smoothing bursty production. (1 native block
  // = 534 samples @ ~32 kHz; *2 for stereo.)
  int16_t sampleBuffer[DSP_SAMPLE_RING * 2];
  uint32_t sampleWrite; // total samples produced (monotonic; index = & mask)
  uint32_t sampleRead;  // total samples consumed (monotonic; index = & mask)
};


Dsp *dsp_init(uint8_t *ram);
void dsp_free(Dsp* dsp);
void dsp_reset(Dsp* dsp);
void dsp_cycle(Dsp* dsp);
uint8_t dsp_read(Dsp* dsp, uint8_t adr);
void dsp_write(Dsp* dsp, uint8_t adr, uint8_t val);
void dsp_getSamples(Dsp* dsp, int16_t* sampleData, int samplesPerFrame);
/* Drop queued host-output samples without rewinding SPC/DSP state. Used only
 * when leaving fast-forward, where buffered samples describe obsolete guest
 * time and retaining them would create persistent A/V latency. */
uint32_t dsp_trimSamples(Dsp* dsp, uint32_t samples_to_keep);
/* Per-voice Gaussian-interpolated sample (canon hardware math). Exposed for the
 * dev-only in-process faithful reference (dsp_shadow) to diff against blargg's
 * reference Gaussian. Pure: reads channel[ch].decodeBuffer[sampleNum..+3], no
 * state change. */
int16_t dsp_getSample(Dsp* dsp, int ch, int sampleNum, int offset);
/* Canonical SNES Gaussian table (byte-identical to blargg's snes9x/bsnes
 * gauss[512]); exposed so the dev faithful reference can apply blargg's exact
 * integer algorithm to it. */
extern const uint16_t gaussValues[512];
void dsp_saveload(Dsp *dsp, SaveLoadInfo *sli);

#endif
