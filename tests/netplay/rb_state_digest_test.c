/*
 * Rollback state-digest invariants.
 *
 * The one that matters most: the S-DSP output ring must not reach the digest.
 * It lives inside the frozen dsp_saveload blob, but its read cursor is moved
 * by the SDL audio thread at the host device's rate — so if it ever folds in,
 * two peers disagree on every single tick and rollback dissolves into a
 * permanent episode storm. That failure looks like a game desync, not like an
 * audio bug, which is exactly why it is worth pinning here.
 *
 * The complementary half is just as important: excluding the ring must not
 * quietly exclude anything else. A skip window one field too wide would hide
 * real DSP divergence instead of reporting it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/snes.h"
#include "netplay/snes_state_digest.h"

/*
 * Host seams the APU pulls in. This harness builds apu/spc/dsp only, so the
 * rest of the machine — and the audio trace rings — are stubbed out. The
 * digest entry point under test (snes_state_digest_apu_of) takes its Apu
 * explicitly and never touches g_snes; the other partitions do, and are not
 * exercised here.
 */
Snes *g_snes;
uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;

void cpu_saveload(Cpu *cpu, SaveLoadInfo *sli) { (void)cpu; (void)sli; }
void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli) { (void)ppu; (void)sli; }
void dma_saveload(Dma *dma, SaveLoadInfo *sli) { (void)dma; (void)sli; }
void cart_saveload(Cart *cart, SaveLoadInfo *sli) { (void)cart; (void)sli; }

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
  (void)port; (void)value;
}
void audio_trace_on_spc_port_read(uint8_t port, uint8_t value) {
  (void)port; (void)value;
}
void audio_trace_on_spc_port_write(uint8_t port, uint8_t value) {
  (void)port; (void)value;
}
void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill) {
  (void)left; (void)right; (void)dropped; (void)ring_fill;
}
void audio_trace_on_reg_write(uint8_t address, uint8_t value) {
  (void)address; (void)value;
}
void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after) {
  (void)read_index; (void)count; (void)available_after;
}
void audio_trace_on_faithful_div(double divergence) { (void)divergence; }
void audio_trace_on_brr_compare(uint16_t block, uint8_t header, uint8_t sample,
                                int canon, int reference, int old, int older) {
  (void)block; (void)header; (void)sample; (void)canon;
  (void)reference; (void)old; (void)older;
}
void audio_trace_on_echo_div(double divergence) { (void)divergence; }
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *out_left, int *out_right) {
  (void)shadow; (void)dsp;
  *out_left = canonical_left;
  *out_right = canonical_right;
}

static int g_failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_failures++;
}

int main(void)
{
    Apu *apu = apu_init();
    uint32_t base, after;

    if (!apu) {
        fprintf(stderr, "apu_init failed\n");
        return 1;
    }
    apu_reset(apu);
    base = snes_state_digest_apu_of(apu);
    check(base != 0u, "apu partition digest is non-trivial");

    /* --- the ring is host state: moving it must not move the digest --- */

    apu->dsp->sampleWrite += 1234u;
    check(snes_state_digest_apu_of(apu) == base,
          "producer cursor (sampleWrite) is excluded");

    apu->dsp->sampleRead += 567u;
    check(snes_state_digest_apu_of(apu) == base,
          "consumer cursor (sampleRead) is excluded");

    memset(apu->dsp->sampleBuffer, 0x5A, sizeof(apu->dsp->sampleBuffer));
    check(snes_state_digest_apu_of(apu) == base,
          "output sample buffer is excluded");

    /* --- everything else in the APU is simulation and must fold in --- */

    apu->dsp->ram[0x0C] ^= 0xFFu; /* MVOLL mirror */
    after = snes_state_digest_apu_of(apu);
    check(after != base, "DSP register file is included");
    apu->dsp->ram[0x0C] ^= 0xFFu;
    check(snes_state_digest_apu_of(apu) == base, "DSP register restore is exact");

    apu->dsp->noiseCounter ^= 0xABCDu;
    check(snes_state_digest_apu_of(apu) != base,
          "DSP state before the ring is included");
    apu->dsp->noiseCounter ^= 0xABCDu;

    apu->dsp->firBufferR[7] ^= 0x1234;
    check(snes_state_digest_apu_of(apu) != base,
          "the field immediately preceding the ring is included");
    apu->dsp->firBufferR[7] ^= 0x1234;
    check(snes_state_digest_apu_of(apu) == base,
          "digest returns to baseline after restore");

    apu->ram[0x0100] ^= 0xFFu;
    check(snes_state_digest_apu_of(apu) != base, "APU RAM is included");
    apu->ram[0x0100] ^= 0xFFu;

    apu->outPorts[2] ^= 0xFFu;
    check(snes_state_digest_apu_of(apu) != base, "APU ports are included");
    apu->outPorts[2] ^= 0xFFu;

    apu->spc->a ^= 0xFFu;
    check(snes_state_digest_apu_of(apu) != base, "SPC700 registers are included");
    apu->spc->a ^= 0xFFu;

    check(snes_state_digest_apu_of(apu) == base, "final state matches baseline");

    /* --- ring save/restore keeps the live consumer where it was --- */
    {
        DspOutputRing saved;
        uint32_t read_before;

        apu->dsp->sampleWrite = 9000u;
        apu->dsp->sampleRead = 8000u;
        dsp_output_ring_save(apu->dsp, &saved);
        read_before = apu->dsp->sampleRead;

        /* Stand in for a snapshot load stomping the ring with stale values. */
        apu->dsp->sampleWrite = 11u;
        apu->dsp->sampleRead = 7u;
        dsp_output_ring_restore(apu->dsp, &saved);
        check(apu->dsp->sampleRead == read_before,
              "ring restore puts the consumer cursor back");
        check(apu->dsp->sampleWrite == 9000u,
              "ring restore puts the producer cursor back");

        /* Resim discard: rewind the producer only. */
        apu->dsp->sampleWrite = 9200u;
        dsp_output_ring_set_write(apu->dsp, 9000u);
        check(apu->dsp->sampleWrite == 9000u, "producer rewind drops resim audio");
        check(apu->dsp->sampleRead == 8000u, "producer rewind leaves the consumer");

        /* Never behind the consumer: dsp_available is an unsigned difference,
         * so a producer pulled below sampleRead would report ~4 billion. */
        dsp_output_ring_set_write(apu->dsp, 100u);
        check(apu->dsp->sampleWrite == apu->dsp->sampleRead,
              "producer rewind clamps at the consumer cursor");
        check(dsp_available(apu->dsp) == 0u, "clamped ring reports empty");
    }

    apu_free(apu);
    printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
