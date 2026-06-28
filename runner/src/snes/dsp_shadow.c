// dsp_shadow.c — see dsp_shadow.h.

#include "dsp_shadow.h"

#include <stdio.h>
#include <stdlib.h>

#include "dsp.h"
#include "../audio_trace.h"

DspShadow* dsp_shadow_create(void) {
  DspShadow* sh = (DspShadow*)calloc(1, sizeof(DspShadow));
  if (!sh) return NULL;
  shadow_verifier_init(&sh->vf);
  const char* e = getenv("SNESRECOMP_AUDIO_SHADOW");
  sh->enabled = (e && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
  if (sh->enabled) {
    fprintf(stderr, "[audio] SNES S-DSP shadow mixer ARMED (verified-"
                    "enhancement; reverts to hardware mix on divergence)\n");
  }
  return sh;
}

void dsp_shadow_free(DspShadow* sh) { free(sh); }

// Catmull-Rom cubic interpolating between p1 and p2 at t in [0,1).
static float cubic(float p0, float p1, float p2, float p3, float t) {
  float a = 2.0f * p1;
  float b = -p0 + p2;
  float c = 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3;
  float d = -p0 + 3.0f * p1 - 3.0f * p2 + p3;
  return 0.5f * (a + b * t + c * t * t + d * t * t * t);
}

// Re-render the dry voice mix with cubic (vs hardware Gaussian) interpolation,
// in float, applying the SAME envelope gain, channel volumes, and master volume
// the canon path used. The verifier's auto-gain absorbs any constant scale
// difference, so only structure must match. Factored out so the always-on tone
// measurement (dev) and the opt-in substitution share one renderer.
static void shadow_render_dry(DspShadow* sh, Dsp* dsp, float* dryLOut,
                              float* dryROut) {
  float vgain = shadow_verifier_gain(&sh->vf);
  float dryL = 0.0f, dryR = 0.0f;
  for (int ch = 0; ch < 8; ++ch) {
    DspChannel* c = &dsp->channel[ch];
    float s;
    if (c->useNoise) {
      s = (float)dsp->noiseSample;
    } else {
      int sampleNum = c->pitchCounter >> 12;       // 0..15 within the block
      int offset = (c->pitchCounter >> 4) & 0xff;  // 8-bit fractional phase
      float t = (float)offset / 256.0f;
      // Canon Gaussian moves olds(buf[n+2]) -> news(buf[n+3]) as offset 0->255;
      // interpolate the same segment with a cubic (no right neighbor: clamp).
      float p0 = (float)c->decodeBuffer[sampleNum + 1];
      float p1 = (float)c->decodeBuffer[sampleNum + 2];
      float p2 = (float)c->decodeBuffer[sampleNum + 3];
      s = cubic(p0, p1, p2, p2, t) * 0.5f;  // *0.5 matches canon getSample >>1
    }
    float voice = s * ((float)c->gain / 2048.0f);   // canon: (s*gain)>>11
    dryL += voice * ((float)c->volumeL / 64.0f);     // canon: (.*vol)>>6
    dryR += voice * ((float)c->volumeR / 64.0f);
  }
  dryL *= ((float)dsp->masterVolumeL / 128.0f) * vgain;  // canon: (.*mvol)>>7
  dryR *= ((float)dsp->masterVolumeR / 128.0f) * vgain;
  *dryLOut = dryL;
  *dryROut = dryR;
}

#if defined(SNESRECOMP_TRACE)
// blargg (snes9x/bsnes) reference Gaussian, applied to the canonical gaussValues
// table (verified byte-identical to blargg's gauss[512]). The ONLY difference
// vs canon dsp_getSample: blargg shifts >>11 per term with the intermediate
// (int16) truncation at the >>11 scale and clears the result LSB; canon shifts
// >>10, truncates at the >>10 scale, then >>1. Same indices, same table, same
// scale -- so the per-voice diff isolates exactly that rounding choice.
static int ref_gauss(const int16_t* buf, int n, int offset) {
  int in0 = buf[n], in1 = buf[n + 1], in2 = buf[n + 2], in3 = buf[n + 3];
  int out  = (gaussValues[0x0ff - offset] * in0) >> 11;
  out += (gaussValues[0x1ff - offset] * in1) >> 11;
  out += (gaussValues[0x100 + offset] * in2) >> 11;
  out  = (int16_t)out;  // intermediate 16-bit truncation at the >>11 scale
  out += (gaussValues[offset] * in3) >> 11;
  if (out < -0x8000) out = -0x8000; else if (out > 0x7fff) out = 0x7fff;
  out &= ~1;            // blargg clears the output LSB
  return out;
}
#endif

void dsp_shadow_process(DspShadow* sh, Dsp* dsp, int canonL, int canonR,
                        int* outL, int* outR) {
  *outL = canonL;
  *outR = canonR;
  if (!sh) return;
#if !defined(SNESRECOMP_TRACE)
  // Production: only pay the re-render cost when the enhancement is armed.
  if (!sh->enabled) return;
#endif
  // Dev (SNESRECOMP_TRACE) ALWAYS renders the reference + records the in-process
  // tone divergence, independent of substitution — the artifact-free internal
  // oracle (no cross-process resample). Substitution below is still opt-in, so
  // with the enhancement off the output stays byte-identical to canon.
  float dryL, dryR;
  shadow_render_dry(sh, dsp, &dryL, &dryR);

#if defined(SNESRECOMP_TRACE)
  // Accumulate only on non-silent canon samples so the RMS reflects active
  // audio rather than boot/silence. Normalized to [-1,1] by 16-bit full scale.
  if (canonL != 0 || canonR != 0) {
    audio_trace_on_shadow_div((double)(canonL - (int)dryL) / 32768.0,
                              (double)(canonR - (int)dryR) / 32768.0);
  }
  // FAITHFUL reference: per active (non-noise, audible) voice, diff canon's
  // hardware Gaussian (dsp_getSample) against blargg's snes9x/bsnes reference
  // Gaussian on the same samples. Isolates the recomp's interpolation arithmetic
  // vs the gold-standard, in-process (no cross-process resample artifact).
  for (int ch = 0; ch < 8; ++ch) {
    DspChannel* c = &dsp->channel[ch];
    if (c->useNoise || c->gain == 0) continue;
    int sampleNum = c->pitchCounter >> 12;
    int offset = (c->pitchCounter >> 4) & 0xff;
    int canon = dsp_getSample(dsp, ch, sampleNum, offset);
    int ref = ref_gauss(c->decodeBuffer, sampleNum, offset);
    audio_trace_on_faithful_div((double)(canon - ref) / 32768.0);
  }
#endif

  // Differential self-check vs the canon dry mix (drives auto-gain + proving).
  shadow_verifier_judge(&sh->vf, (float)canonL / 32768.0f,
                        (float)canonR / 32768.0f, dryL / 32768.0f,
                        dryR / 32768.0f);
  if (sh->vf.reverted[0]) {
    fprintf(stderr, "[audio] SNES S-DSP shadow DEGRADED: %s\n", sh->vf.reverted);
    sh->vf.reverted[0] = '\0';
  }

  // Substitute the better render only when explicitly armed AND proven.
  if (sh->enabled && shadow_verifier_proven(&sh->vf)) {
    int oL = (int)dryL, oR = (int)dryR;
    *outL = oL < -0x8000 ? -0x8000 : (oL > 0x7fff ? 0x7fff : oL);
    *outR = oR < -0x8000 ? -0x8000 : (oR > 0x7fff ? 0x7fff : oR);
  }
}
