#ifndef SNES_STATE_DIGEST_H
#define SNES_STATE_DIGEST_H

/*
 * Partitioned simulation-state digests for rollback hash_confirm /
 * FRAME_COMMIT / baseline / POST.
 *
 * Two rules govern what may enter a digest, and both come from
 * recomp-ai-rules/NETPLAY.md §1 and §5:
 *
 *   1. Digest the SIMULATION only. Presentation state (the audio output
 *      ring, the window, the renderer) may differ freely between peers.
 *   2. The digest domain must equal the SNAPSHOT domain. State that is
 *      digested but not restored makes every rewind fork; state that is
 *      restored but not digested lets a fork go unseen. Both digests and
 *      snapshots here walk the same `*_saveload` serializers so the two
 *      domains cannot drift apart as the emulator grows fields.
 *
 * The one deliberate subtraction from that shared domain is the S-DSP
 * output ring (`Dsp.sampleBuffer` / `sampleWrite` / `sampleRead`). It sits
 * inside the frozen `dsp_saveload` blob, but its read cursor is advanced by
 * the SDL audio thread at the host device's rate — wall-clock state by
 * definition, and never identical across two peers. SNES_DIGEST_PART_APU
 * folds only the DSP prefix that precedes it. `snes_rb_snapshot_load`
 * makes the matching subtraction on the restore side.
 *
 * Partition 0 is the master digest (every partition folded in order) and is
 * what rides the FRAME_COMMIT wire. The rest exist so a fork reports which
 * subsystem moved: "the state differs" is not a diagnosis, "the APU differs
 * and everything else matches" is.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_DIGEST_PART_MASTER 0u
#define SNES_DIGEST_PART_CPU    1u
#define SNES_DIGEST_PART_WRAM   2u  /* 128 KiB WRAM + $21xx/$42xx tail regs */
#define SNES_DIGEST_PART_APU    3u  /* SPC + APU regs + DSP minus output ring */
#define SNES_DIGEST_PART_PPU    4u  /* PPU regs + VRAM/CGRAM/OAM */
#define SNES_DIGEST_PART_DMA    5u
#define SNES_DIGEST_PART_CART   6u  /* SRAM + SA-1 / Cx4 / DSP-1 */
#define SNES_DIGEST_PART_COUNT  7u

typedef struct SnesStateDigestParts {
    uint32_t cpu;
    uint32_t wram;
    uint32_t apu;
    uint32_t ppu;
    uint32_t dma;
    uint32_t cart;
    uint32_t master;
} SnesStateDigestParts;

/*
 * The APU partition over an explicit Apu, rather than the live g_snes.
 *
 * This is the seam the audio-ring exclusion is tested through: the property
 * that matters ("moving the output ring cannot move the digest, moving a DSP
 * register must") is checkable against a bare apu_init() with no ROM, PPU, or
 * cart in play. Keeping the test on the real function rather than a replica
 * of its offset arithmetic is the point — a replica would keep passing after
 * the thing it mirrors had drifted.
 */
struct Apu;
uint32_t snes_state_digest_apu_of(struct Apu *apu);

/* One partition. Unknown partition ids return 0. */
uint32_t snes_state_digest(uint32_t partition);

/* Every partition in one walk (cheaper than 7 calls). */
void snes_state_digest_parts(SnesStateDigestParts *out);

/* Human-readable partition name for divergence logs ("apu", "ppu", …). */
const char *snes_state_digest_part_name(uint32_t partition);

/* First partition whose digest differs between two parts blocks, or
 * SNES_DIGEST_PART_COUNT when they agree. Used to name the forked
 * subsystem at a baseline/POST mismatch. */
uint32_t snes_state_digest_first_diff(const SnesStateDigestParts *a,
                                      const SnesStateDigestParts *b);

#ifdef __cplusplus
}
#endif

#endif /* SNES_STATE_DIGEST_H */
