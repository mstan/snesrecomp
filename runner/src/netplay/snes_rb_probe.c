/*
 * snes_rb_probe — single-process rollback determinism probe.
 *
 * Rollback assumes one thing: save a snapshot, run N frames, restore it, run
 * the same N frames with the same inputs, and you land on byte-identical
 * state. Nothing in the runner ever checked that. When it is false — because
 * the snapshot omits some host-side execution cursor — the symptom appears
 * only in a two-peer session, as a follower that wedges or desyncs, which is
 * the most expensive place to debug it.
 *
 * Arm with SNESRECOMP_RB_PROBE=<period>[:<depth>] (depth defaults to 3):
 * every `period` guest frames, run the save/replay/restore experiment inline
 * and print the first step and partition that disagree. The probe is
 * transparent to the game: it restores the pre-probe snapshot when it is
 * done, so guest time does not advance and the run continues normally.
 *
 * Offline. Never arm it in a netplay session — the extra frames it simulates
 * are invisible to the peer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/snes.h"
#include "snes_state_digest.h"

extern int snes_frame_counter;
extern CpuState g_cpu;
extern Snes *g_snes;
extern uint64_t g_apu_pace_cycles_estimate;
extern uint64_t g_main_cpu_cycles_estimate;

/* Per-pass cycle accounting, to separate "the frame executed differently"
 * from "the frame was charged differently for the same execution". */
typedef struct {
    uint64_t master, pace, blocks;
    double   catchup;
    uint16_t hpos;
} ProbeCycles;

static void probe_cycles(ProbeCycles *c)
{
    c->master  = g_cpu.master_cycles;
    c->pace    = g_apu_pace_cycles_estimate;
    c->blocks  = g_main_cpu_cycles_estimate;
    c->catchup = g_snes->apuCatchupCycles;
    c->hpos    = g_snes->hPos;
}

#define RB_PROBE_WRAM_LEN 0x20000u

/*
 * Carrier hunt (SNESRECOMP_RB_PROBE_STATICS=1). The replay divergence is
 * deterministic — the same two digests come back on a loopback pair and on a
 * real Linux/Windows match — so the carrier is host state a frame mutates and
 * the snapshot does not restore. Rather than guess which global, image the
 * executable's whole .data+.bss at the identical starting point of two
 * replays and diff. Addresses resolve to symbols with `nm`.
 *
 * GNU-ld only, and deliberately a bisection instrument, not a feature.
 */
#if defined(__linux__) && defined(__GNUC__)
#define RB_PROBE_HAVE_STATICS 1
extern char __data_start[], _edata[], __bss_start[], _end[];
static uint8_t *rb_probe_statics_grab(void)
{
    size_t dn = (size_t)(_edata - __data_start);
    size_t bn = (size_t)(_end - __bss_start);
    uint8_t *b = (uint8_t *)malloc(dn + bn);
    if (b) {
        memcpy(b, __data_start, dn);
        memcpy(b + dn, __bss_start, bn);
    }
    return b;
}
static void rb_probe_statics_diff(const uint8_t *was)
{
    size_t dn = (size_t)(_edata - __data_start);
    size_t bn = (size_t)(_end - __bss_start);
    size_t k, n = 0;
    if (!was)
        return;
    fprintf(stderr, "rb_probe:   [statics base data=%p bss=%p]\n",
            (void *)__data_start, (void *)__bss_start);
    for (k = 0; k < dn; ++k)
        if (was[k] != ((const uint8_t *)__data_start)[k]) {
            if (n < 40)
                fprintf(stderr, "rb_probe:   .data @%p %02X->%02X\n",
                        (void *)(__data_start + k), was[k],
                        ((const uint8_t *)__data_start)[k]);
            n++;
        }
    for (k = 0; k < bn; ++k)
        if (was[dn + k] != ((const uint8_t *)__bss_start)[k]) {
            if (n < 40)
                fprintf(stderr, "rb_probe:   .bss  @%p %02X->%02X\n",
                        (void *)(__bss_start + k), was[dn + k],
                        ((const uint8_t *)__bss_start)[k]);
            n++;
        }
    fprintf(stderr, "rb_probe:   statics differing at replay entry: %zu\n", n);
}
#else
#define RB_PROBE_HAVE_STATICS 0
#endif

#define RB_PROBE_MAX_DEPTH 16

static int   s_period = -1;      /* -1 unread, 0 disarmed */
static int   s_depth  = 3;
static int   s_active;           /* reentrancy: our own RtlRunFrame calls */
static int   s_runs, s_fails;

static void probe_read_env(void)
{
    const char *v = getenv("SNESRECOMP_RB_PROBE");
    const char *colon;

    s_period = 0;
    if (!v || !v[0])
        return;
    s_period = atoi(v);
    if (s_period < 1) { s_period = 0; return; }
    colon = strchr(v, ':');
    if (colon && colon[1]) {
        s_depth = atoi(colon + 1);
        if (s_depth < 1) s_depth = 1;
        if (s_depth > RB_PROBE_MAX_DEPTH) s_depth = RB_PROBE_MAX_DEPTH;
    }
    fprintf(stderr, "rb_probe: armed — every %d frames, replay depth %d "
            "(SNESRECOMP_RB_PROBE)\n", s_period, s_depth);
}

static const char *first_diff_part(const SnesStateDigestParts *a,
                                   const SnesStateDigestParts *b)
{
    if (a->wram != b->wram)
        return snes_state_digest_part_name(SNES_DIGEST_PART_WRAM);
    if (a->apu != b->apu)
        return snes_state_digest_part_name(SNES_DIGEST_PART_APU);
    if (a->ppu != b->ppu)
        return snes_state_digest_part_name(SNES_DIGEST_PART_PPU);
    return "other";
}

/*
 * True once SNESRECOMP_RB_PROBE is set. The runner's offline APU pacing adds
 * SPC cycles from WALL-CLOCK elapsed time when no audio consumer is draining
 * (rtl_sync_apu_frame_boundary); host time in guest state can never survive a
 * rewind, so a netplay session already suppresses it. The probe must be held
 * to the same contract or it only ever rediscovers that.
 */
int snes_rb_probe_armed(void)
{
    if (s_period < 0)
        probe_read_env();
    return s_period != 0;
}

void snes_rb_probe_after_frame(uint32_t inputs)
{
    SnesStateDigestParts pass1[RB_PROBE_MAX_DEPTH];
    SnesStateDigestParts pass2[RB_PROBE_MAX_DEPTH];
    uint8_t *snap;
    uint8_t *wram1 = NULL;
    uint8_t *tail1 = NULL;
    uint8_t *statics2 = NULL;
    size_t   tail_len = sizeof(Snes) - offsetof(Snes, hPos);
    size_t bound, len;
    int i, bad = -1;
    int base_frame;

    if (s_active)
        return;
    if (s_period < 0)
        probe_read_env();
    if (s_period == 0 || snes_frame_counter <= 0 ||
        (snes_frame_counter % s_period) != 0)
        return;

    bound = RtlRollbackSnapshotBound();
    snap = (uint8_t *)malloc(bound);
    if (!snap)
        return;
    len = RtlRollbackSaveToMemory(snap, bound);
    if (len == 0) {
        fprintf(stderr, "rb_probe: FAIL frame=%d — snapshot save returned 0\n",
                snes_frame_counter);
        free(snap);
        return;
    }

    /* Symmetry check: re-saving immediately after a restore should reproduce
     * the same bytes, so anything the capture side records and the apply side
     * forgets shows up as a differing byte WITHOUT needing a replay to expose
     * it — mechanical enforcement of the resync contract in common_rtl.c, and
     * the offset says whether the carrier sits in the guest blob or the host
     * residue appended after it.
     *
     * This used to be opt-in, and was therefore off in every run ever made,
     * because of one INTENTIONAL asymmetry it could not tell from a bug:
     * RtlRollbackLoadFromMemory deliberately lifts the DSP output ring out and
     * keeps the LIVE one, since that ring belongs to the audio consumer and
     * must not rewind. The consumer thread moves it between the two saves, so
     * a hit inside the ring was expected and intermittent, and left on by
     * default the check cried wolf — which is worse than not checking.
     *
     * That rationale turned out to be stale. Measured over 94 probe ticks
     * against a live pulseaudio device (32040 Hz, 2ch, so a real consumer was
     * running): zero asymmetries and, more to the point, zero bytes moving at
     * all. Something already fixed it — most likely the audio lock the armed
     * probe now forces. The check had been gated off for a hazard that no
     * longer occurs, and being opt-in meant off in every run ever made.
     *
     * The control pair below is kept anyway, and is honest about what it is:
     * two consecutive saves with no restore between them differ in exactly the
     * bytes moving under us, so they are the noise floor and differences
     * outside it are the only ones reported. It currently excludes NOTHING.
     * Its value is that the floor becomes observable in every run instead of
     * assumed — if the ring starts moving again, the count says so before the
     * check starts crying wolf, rather than after.
     *
     * SNESRECOMP_RB_PROBE_NO_SYMMETRY=1 turns it off; nothing needs to turn it
     * on any more. */
    if (!getenv("SNESRECOMP_RB_PROBE_NO_SYMMETRY")) {
        uint8_t *ctrl = (uint8_t *)malloc(bound);
        uint8_t *reblob = (uint8_t *)malloc(bound);
        if (ctrl && reblob) {
            size_t ctrl_len = RtlRollbackSaveToMemory(ctrl, bound);
            size_t relen = 0;
            size_t volatile_n = 0;
            if (RtlRollbackLoadFromMemory(snap, len))
                relen = RtlRollbackSaveToMemory(reblob, bound);
            if (ctrl_len == len) {
                size_t k;
                for (k = 0; k < len; ++k)
                    if (snap[k] != ctrl[k])
                        volatile_n++;
            }
            if (relen != len) {
                fprintf(stderr, "rb_probe: ASYMMETRIC frame=%d — re-save is "
                        "%zu bytes, original %zu\n",
                        snes_frame_counter, relen, len);
            } else {
                size_t k;
                size_t hits = 0;
                for (k = 0; k < len; ++k) {
                    if (snap[k] == reblob[k])
                        continue;
                    /* Moving under us in the control pair too: not a restore
                     * asymmetry, just a field the consumer owns. */
                    if (ctrl_len == len && snap[k] != ctrl[k])
                        continue;
                    if (hits++ == 0)
                        fprintf(stderr,
                                "rb_probe: ASYMMETRIC frame=%d — byte +%zu of "
                                "%zu differs after restore (%02X->%02X, %s) "
                                "and is NOT volatile (%zu volatile bytes were "
                                "excluded). A field is captured but not "
                                "applied; see the resync contract in "
                                "common_rtl.c\n",
                                snes_frame_counter, k, len, snap[k], reblob[k],
                                (len - k) <= 1200u ? "host residue tail"
                                                   : "guest blob",
                                volatile_n);
                }
                if (hits == 0 && getenv("SNESRECOMP_RB_PROBE_VERBOSE"))
                    fprintf(stderr, "rb_probe: symmetric frame=%d (%zu "
                            "volatile bytes excluded)\n",
                            snes_frame_counter, volatile_n);
            }
        }
        free(ctrl);
        free(reblob);
    }

    /* Round-trip check: if restoring the snapshot does not reproduce
     * the state it was taken from, the replay experiment below is measuring
     * a lossy save, not a nondeterministic frame. */
    {
        SnesStateDigestParts at_save, at_restore;
        snes_state_digest_parts(&at_save);
        if (!RtlRollbackLoadFromMemory(snap, len)) {
            fprintf(stderr, "rb_probe: FAIL frame=%d — round-trip restore "
                    "refused\n", snes_frame_counter);
            free(snap);
            return;
        }
        snes_state_digest_parts(&at_restore);
        if (at_save.master != at_restore.master) {
            unsigned k;
            const uint8_t *a = (const uint8_t *)&at_save;
            (void)a;
            fprintf(stderr,
                    "rb_probe: LOSSY SNAPSHOT frame=%d partition=%s "
                    "saved=%08x restored=%08x (wram %08x/%08x apu %08x/%08x "
                    "ppu %08x/%08x)\n",
                    snes_frame_counter,
                    first_diff_part(&at_save, &at_restore),
                    (unsigned)at_save.master, (unsigned)at_restore.master,
                    (unsigned)at_save.wram, (unsigned)at_restore.wram,
                    (unsigned)at_save.apu, (unsigned)at_restore.apu,
                    (unsigned)at_save.ppu, (unsigned)at_restore.ppu);
            (void)k;
        }
    }

    base_frame = snes_frame_counter;
    /* Keep every step's WRAM so a divergence can name the guest address that
     * moved. "wram differs" is a symptom; the byte is the lead. */
    if (g_cpu.ram)
        wram1 = (uint8_t *)malloc((size_t)s_depth * RB_PROBE_WRAM_LEN);
    tail1 = (uint8_t *)malloc((size_t)s_depth * tail_len);
    s_active = 1;
    for (i = 0; i < s_depth; ++i) {
        RtlRunFrame(inputs);
        snes_state_digest_parts(&pass1[i]);
        if (wram1)
            memcpy(wram1 + (size_t)i * RB_PROBE_WRAM_LEN, g_cpu.ram,
                   RB_PROBE_WRAM_LEN);
        if (tail1)
            memcpy(tail1 + (size_t)i * tail_len,
                   (const uint8_t *)&g_snes->hPos, tail_len);
    }
    if (!RtlRollbackLoadFromMemory(snap, len)) {
        fprintf(stderr, "rb_probe: FAIL frame=%d — snapshot restore refused\n",
                base_frame);
        s_active = 0;
        free(snap);
        free(wram1);
        free(tail1);
        return;
    }
#if RB_PROBE_HAVE_STATICS
    if (getenv("SNESRECOMP_RB_PROBE_STATICS"))
        statics2 = rb_probe_statics_grab();
#endif
    for (i = 0; i < s_depth; ++i) {
        RtlRunFrame(inputs);
        snes_state_digest_parts(&pass2[i]);
        if (bad < 0 && pass1[i].master != pass2[i].master)
            bad = i;
    }
    /* Third pass from the same snapshot. pass3 starts from the same restored
     * bytes as pass2 but inherits whatever pass2 left in host statics, so
     * pass2 != pass3 localises the carrier: unrestored host state that the
     * frame itself mutates. pass2 == pass3 with both != pass1 means the
     * carrier was set before the probe rather than by a replayed frame.
     * (The audio consumer thread is ruled out by A/B: muting RtlRenderAudio
     * for a whole run changed the divergence count by zero.) */
    {
        SnesStateDigestParts p3;
        ProbeCycles c0, c1;
        int same23 = 1;
        if (RtlRollbackLoadFromMemory(snap, len)) {
#if RB_PROBE_HAVE_STATICS
            if (bad >= 0)
                rb_probe_statics_diff(statics2);
#endif
            probe_cycles(&c0);
            s_active = 1;
            for (i = 0; i < s_depth; ++i) {
                RtlRunFrame(inputs);
                snes_state_digest_parts(&p3);
                if (p3.master != pass2[i].master) same23 = 0;
            }
            s_active = 0;
            probe_cycles(&c1);
            if (bad >= 0)
                fprintf(stderr,
                        "rb_probe:   pass3 charged: master+%llu pace+%llu "
                        "blocks+%llu catchup %.3f->%.3f hPos %u->%u\n",
                        (unsigned long long)(c1.master - c0.master),
                        (unsigned long long)(c1.pace - c0.pace),
                        (unsigned long long)(c1.blocks - c0.blocks),
                        c0.catchup, c1.catchup,
                        (unsigned)c0.hpos, (unsigned)c1.hpos);
            if (bad >= 0)
                fprintf(stderr, "rb_probe:   pass2==pass3: %s\n",
                        same23 ? "YES (carrier predates the probe)"
                               : "NO (a replayed frame mutates unrestored "
                                 "host state)");
        }
    }

    /* Land back where the caller left off: the probe must not consume guest
     * time, or the game it is measuring runs at the wrong speed. */
    s_runs++;
    if (bad >= 0) {
        s_fails++;
        fprintf(stderr,
                "rb_probe: DIVERGED frame=%d step=%d/%d partition=%s "
                "first=%08x replay=%08x (fail %d of %d probes)\n",
                base_frame, bad + 1, s_depth,
                first_diff_part(&pass1[bad], &pass2[bad]),
                (unsigned)pass1[bad].master, (unsigned)pass2[bad].master,
                s_fails, s_runs);
        if (wram1 && pass1[bad].wram != pass2[bad].wram) {
            /* pass2 left the machine at its last step, so only the final
             * step's WRAM is still live to compare against. */
            if (bad == s_depth - 1) {
                const uint8_t *a = wram1 + (size_t)bad * RB_PROBE_WRAM_LEN;
                const uint8_t *b = g_cpu.ram;
                unsigned n = 0, first = 0;
                unsigned k;
                for (k = 0; k < RB_PROBE_WRAM_LEN; ++k) {
                    if (a[k] != b[k]) {
                        if (!n) first = k;
                        n++;
                    }
                }
                fprintf(stderr,
                        "rb_probe:   wram bytes differ=%u first=$7E:%04X "
                        "(bank %u) run=%02X replay=%02X\n",
                        n, (unsigned)(first & 0xFFFFu), (unsigned)(first >> 16),
                        a[first], b[first]);
            } else {
                fprintf(stderr, "rb_probe:   (re-run with depth=%d to dump the "
                        "differing WRAM bytes)\n", bad + 1);
            }
        }
        /* The "wram" partition also covers the Snes struct tail (beam
         * position, timers, IRQ latches, DMA) — identical guest memory with a
         * different machine clock reads as a wram fork. Name the offset. */
        if (tail1 && bad == s_depth - 1) {
            const uint8_t *a = tail1 + (size_t)bad * tail_len;
            const uint8_t *b = (const uint8_t *)&g_snes->hPos;
            size_t k, n = 0, first = 0;
            for (k = 0; k < tail_len; ++k) {
                if (a[k] != b[k]) { if (!n) first = k; n++; }
            }
            fprintf(stderr,
                    "rb_probe:   Snes tail bytes differ=%u/%u first=+0x%zx "
                    "(offsetof=0x%zx) run=%02X replay=%02X\n",
                    (unsigned)n, (unsigned)tail_len, first,
                    first + offsetof(Snes, hPos),
                    n ? a[first] : 0, n ? b[first] : 0);
            if (n) {
                fprintf(stderr, "rb_probe:   tail offsets:");
                for (k = 0; k < tail_len; ++k)
                    if (a[k] != b[k])
                        fprintf(stderr, " +%zu(%02X/%02X)", k, a[k], b[k]);
                fprintf(stderr, "\n");
            }
        }
    }
    /* Land back where the caller left off: the probe must not consume guest
     * time, or the game it is measuring runs at the wrong speed. */
    (void)RtlRollbackLoadFromMemory(snap, len);
    s_active = 0;
    free(snap);
    free(wram1);
    free(tail1);
    free(statics2);
}
