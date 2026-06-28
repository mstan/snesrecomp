// dsp_shadow.h — S-DSP HLE shadow mixer (verified-enhancement; default off).
//
// The canon emulated S-DSP (dsp.c) keeps running and stays the authoritative
// output AND the verify oracle. This shadow re-renders the same BRR voices
// with better-than-Gaussian (cubic) interpolation in float — removing the
// hardware's characteristic treble muffling and per-channel 16-bit truncation
// — and the engine-agnostic ShadowVerifier polices it against the canon dry
// mix every output sample. It substitutes only after a proven window and
// reverts loudly (DEGRADED) on divergence. Off unless SNESRECOMP_AUDIO_SHADOW
// is set; with it off the output is byte-identical. See PRINCIPLES carve-out
// and docs/SHADOW_ENHANCEMENTS.md.
//
// The verifier auto-calibrates a constant gain, so the shadow's absolute scale
// need not exactly match the canon — only its structure (relative levels +
// timing). A mis-phased render just fails the check and falls back to canon.

#ifndef SNESRECOMP_DSP_SHADOW_H
#define SNESRECOMP_DSP_SHADOW_H

#include "audio_shadow.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Dsp Dsp;

typedef struct DspShadow {
  ShadowVerifier vf;
  int enabled;
} DspShadow;

// Allocate + init; reads SNESRECOMP_AUDIO_SHADOW (default off; "0" forces off).
DspShadow* dsp_shadow_create(void);
void dsp_shadow_free(DspShadow* sh);

// Given the canon dry mix (post master-volume, pre-echo, 16-bit), recompute
// the shadow dry mix from the live channel state, feed both to the verifier,
// and write the chosen dry into *outL/*outR (the shadow only when proven).
void dsp_shadow_process(DspShadow* sh, Dsp* dsp, int canonL, int canonR,
                        int* outL, int* outR);

// Dev faithful-reference (SNESRECOMP_TRACE): re-decode one BRR block with
// blargg's snes9x/bsnes algorithm from ARAM at `blockStart` seeded by the two
// previous canon samples, and record the canon-vs-reference divergence into
// audio_trace (brr_div). `canonOut16` points at canon's just-decoded 16 samples
// (decodeBuffer+3). No-op outside trace builds (the call site is guarded).
void dsp_shadow_verify_brr(const uint8_t* aram, uint16_t blockStart,
                           int oldSeed, int olderSeed, const int16_t* canonOut16);

// Dev faithful-reference (SNESRECOMP_TRACE): recompute the echo 8-tap FIR with
// blargg's snes9x/bsnes algorithm on canon's FIR history + coefficients, and
// record the canon-vs-reference divergence (echo_div). No-op outside trace.
void dsp_shadow_verify_echo(const int16_t* firL, const int16_t* firR,
                            const int8_t* coeff, int idx,
                            int canonSumL, int canonSumR);

#ifdef __cplusplus
}
#endif

#endif  // SNESRECOMP_DSP_SHADOW_H
