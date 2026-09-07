#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SNES_COSIM
#include "cpu_state.h"
CpuState g_cpu;
#endif

#include "audio_trace.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
int snes_frame_counter;

static int failures;

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "dsp_trace_gate_test: %s\n", message);
    failures++;
  }
}

static void set_shadow_env(const char *value) {
#ifdef _WIN32
  _putenv_s("SNESRECOMP_AUDIO_SHADOW", value ? value : "");
#else
  if (value) setenv("SNESRECOMP_AUDIO_SHADOW", value, 1);
  else unsetenv("SNESRECOMP_AUDIO_SHADOW");
#endif
}

static void seed_dsp(Dsp *dsp) {
  dsp_reset(dsp);
  dsp->mute = false;
  dsp->reset = false;
  dsp->masterVolumeL = 127;
  dsp->masterVolumeR = 127;
  dsp->echoVolumeL = 0;
  dsp->echoVolumeR = 0;
  dsp->feedbackVolume = 0;
  dsp->echoWrites = false;
  dsp->echoDelay = 1;
  dsp->echoRemain = 1;

  DspChannel *ch = &dsp->channel[0];
  ch->volumeL = 64;
  ch->volumeR = 64;
  ch->gain = 2047;
  ch->pitch = 0;
  ch->pitchCounter = 0x0800;
  ch->useNoise = false;
  for (int i = 0; i < 19; i++)
    ch->decodeBuffer[i] = (int16_t)(-12000 + i * 1500);
}

static void test_disabled_shadow_allocation(void) {
  set_shadow_env(NULL);
  DspShadow *shadow = dsp_shadow_create();
#if SNESRECOMP_DSP_FORENSICS
  check(shadow != NULL, "forensic builds keep disabled shadow verifier");
  check(shadow && !shadow->enabled, "disabled forensic shadow is not armed");
#else
  check(shadow == NULL, "production disabled shadow is not allocated");
#endif
  dsp_shadow_free(shadow);
}

static void test_direct_forensic_counters(void) {
  uint8_t aram[0x10000] = {0};
  int16_t canon[16] = {0};
  int16_t fir_l[8] = {0};
  int16_t fir_r[8] = {0};
  int8_t coeff[8] = {0};
  AudioTraceStats before, after;

  aram[0x200] = 0x20;
  for (int i = 0; i < 8; i++)
    aram[0x201 + i] = (uint8_t)(0x11u * (uint8_t)(i + 1));
  for (int i = 0; i < 16; i++)
    canon[i] = (int16_t)(i * 16);
  fir_l[0] = 1000;
  fir_r[0] = -1000;
  coeff[7] = 64;

  audio_trace_get_stats(&before);
  dsp_shadow_verify_brr(aram, 0x200, 0, 0, canon);
  dsp_shadow_verify_echo(fir_l, fir_r, coeff, 0, 1000, -1000);
  audio_trace_get_stats(&after);

#if SNESRECOMP_DSP_FORENSICS
  check(after.brr_div_count - before.brr_div_count == 16,
        "forensics record 16 BRR comparison samples");
  check(after.echo_div_count - before.echo_div_count == 2,
        "forensics record stereo echo comparison");
#else
  check(after.brr_div_count == before.brr_div_count,
        "production TRACE=0 skips BRR comparison");
  check(after.echo_div_count == before.echo_div_count,
        "production TRACE=0 skips echo comparison");
#endif
}

static void test_disabled_shadow_preserves_pcm_and_state(void) {
  uint8_t aram_a[0x10000] = {0};
  uint8_t aram_b[0x10000] = {0};
  set_shadow_env(NULL);

  Dsp *live = dsp_init(aram_a);
  Dsp *ref = dsp_init(aram_b);
  check(live != NULL && ref != NULL, "dsp_init allocated test DSPs");
  if (!live || !ref) return;

  dsp_shadow_free((DspShadow *)ref->shadow);
  ref->shadow = NULL;
  seed_dsp(live);
  seed_dsp(ref);

  dsp_cycle(live);
  dsp_cycle(ref);

  check(live->sampleWrite == ref->sampleWrite, "sample count unchanged");
  check(live->sampleBuffer[0] == ref->sampleBuffer[0],
        "left PCM unchanged by disabled shadow");
  check(live->sampleBuffer[1] == ref->sampleBuffer[1],
        "right PCM unchanged by disabled shadow");
  check(live->sampleBuffer[0] != 0 || live->sampleBuffer[1] != 0,
        "PCM comparison exercises non-silent output");

  size_t save_off = offsetof(Dsp, ram);
  check(memcmp((const unsigned char *)live + save_off,
               (const unsigned char *)ref + save_off,
               sizeof(Dsp) - save_off) == 0,
        "serialized DSP region unchanged by disabled diagnostics");

  dsp_free(live);
  dsp_free(ref);
}

static void test_armed_shadow_still_verifies(void) {
  uint8_t aram[0x10000] = {0};
  AudioTraceStats before, after;
  set_shadow_env("1");
  Dsp *dsp = dsp_init(aram);
  check(dsp != NULL, "armed dsp_init allocated DSP");
  if (!dsp) return;
  check(dsp->shadow != NULL, "armed shadow allocates in every build mode");
  check(dsp->shadow && ((DspShadow *)dsp->shadow)->enabled,
        "armed shadow marks enhancement enabled");
  seed_dsp(dsp);

  DspShadow *shadow = (DspShadow *)dsp->shadow;
  int out_l = 1234;
  int out_r = -567;
  audio_trace_get_stats(&before);
  dsp_shadow_process(shadow, dsp, out_l, out_r, &out_l, &out_r);
  audio_trace_get_stats(&after);
  check(shadow->vf.check.phase == 1, "armed shadow feeds verifier");
#if SNESRECOMP_DSP_FORENSICS
  check(after.shadow_div_count - before.shadow_div_count == 1,
        "forensics record shadow dry-mix divergence");
  check(after.faithful_div_count - before.faithful_div_count == 1,
        "forensics record faithful Gaussian divergence");
#else
  check(after.shadow_div_count == before.shadow_div_count,
        "production enhancement skips shadow-div diagnostics");
  check(after.faithful_div_count == before.faithful_div_count,
        "production enhancement skips faithful Gaussian diagnostics");
#endif

  dsp_free(dsp);
  set_shadow_env(NULL);
}

int main(void) {
  check(offsetof(Dsp, shadow) < offsetof(Dsp, ram),
        "shadow pointer remains outside dsp_saveload region");
  test_disabled_shadow_allocation();
  test_direct_forensic_counters();
  test_disabled_shadow_preserves_pcm_and_state();
  test_armed_shadow_still_verifies();
  if (failures) return 1;
  printf("dsp_trace_gate_test: PASS forensics=%d\n", SNESRECOMP_DSP_FORENSICS);
  return 0;
}
