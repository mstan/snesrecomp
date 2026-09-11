/* snes_runahead.c — see snes_runahead.h. */

#include "snes_runahead.h"

#include <stdio.h>
#include <stdlib.h>

#include "common_rtl.h"
#include "snes/snes.h"
#include "common_cpu_infra.h"   /* g_snes */
#include "netplay/snes_netplay.h"

static int      s_frames;
static uint8_t *s_blob;
static size_t   s_cap;
static int      s_warned;

/* Generous first-probe ceiling; the real snapshot size replaces it, exactly
 * as snes_rewind.c does. */
#define RA_PROBE_CAP (2u * 1024u * 1024u)

void snes_runahead_set_frames(int frames)
{
    if (frames < 0) frames = 0;
    if (frames > SNES_RUNAHEAD_MAX) frames = SNES_RUNAHEAD_MAX;
    if (frames == s_frames) return;
    s_frames = frames;
    fprintf(stderr, "[runahead] %d frame(s)%s\n", s_frames,
            s_frames ? "" : " (off)");
}

int snes_runahead_frames(void) { return s_frames; }

void snes_runahead_configure(void)
{
    const char *env = getenv("SNESRECOMP_RUNAHEAD");
    if (env && *env)
        snes_runahead_set_frames(atoi(env));
}

int snes_runahead_active(void)
{
    if (s_frames <= 0) return 0;
    if (!g_snes) return 0;
    /* Offline only. See the header: two peers cannot each speculate about a
     * shared timeline, and netplay owns this same rollback machinery. */
#if defined(SNESRECOMP_NET)
    if (snes_netplay_active()) return 0;
#endif
    return 1;
}

void snes_runahead_shutdown(void)
{
    free(s_blob);
    s_blob = NULL;
    s_cap = 0;
}

static int ensure_blob(void)
{
    size_t n;
    uint8_t *probe;

    if (s_blob) return 1;

    probe = (uint8_t *)malloc(RA_PROBE_CAP);
    if (!probe) return 0;
    n = RtlRollbackSaveToMemory(probe, RA_PROBE_CAP);
    free(probe);
    if (!n) return 0;              /* machine not ready yet; try again later */

    /* Headroom, for the same reason the rewind ring keeps some: a snapshot can
     * legitimately differ in size between frames, and one that does not fit is
     * a speculation lost. */
    s_cap = n + (n / 8) + 4096;
    s_blob = (uint8_t *)malloc(s_cap);
    if (!s_blob) { s_cap = 0; return 0; }
    fprintf(stderr, "[runahead] snapshot is %zu bytes\n", n);
    return 1;
}

int snes_runahead_run_frame(uint32_t inputs)
{
    size_t len;
    uint32_t audio_cursor;
    int saved_counter, i;
    bool saved_render;

    if (!snes_runahead_active()) return 0;
    if (!ensure_blob()) {
        if (!s_warned) {
            fprintf(stderr, "[runahead] no snapshot buffer; running normally\n");
            s_warned = 1;
        }
        return 0;
    }

    /*
     * Frame 0 is the real one: real input, real audio, and rendered normally.
     * Rendering it is a few hundred microseconds that the last speculative
     * frame then overwrites -- deliberate, because if the snapshot below fails
     * we still have a correct picture to show rather than a black frame.
     */
    RtlRunFrame(inputs);

    len = RtlRollbackSaveToMemory(s_blob, s_cap);
    if (!len) {
        /* Advanced correctly, just without speculation. Not an error worth
         * spamming: a snapshot can fail transiently while the machine settles. */
        return 1;
    }

    /* Everything below must leave no trace except the picture. */
    saved_render  = g_snes->disableRender;
    saved_counter = snes_frame_counter;
    audio_cursor  = RtlAudioProducerCursor();

    RtlSetSpeculativeFrame(true);
    for (i = 1; i <= s_frames; i++) {
        /* Only the last speculative frame is composited -- it is the one the
         * player sees. The others exist solely to carry the guest forward. */
        g_snes->disableRender = (i < s_frames);
        RtlRunFrame(inputs);
    }
    RtlSetSpeculativeFrame(false);

    /* Audio produced by speculation is thrown away; frame 0 already produced
     * this tick's real samples. The DSP output ring itself belongs to the live
     * audio thread and is deliberately NOT rewound by the rollback load. */
    RtlAudioRewindProducer(audio_cursor);

    if (!RtlRollbackLoadFromMemory(s_blob, len)) {
        /* The guest is now N frames ahead of where the host thinks it is, and
         * there is no way back. Loud, and run-ahead turns itself off rather
         * than corrupting every subsequent frame the same way. */
        fprintf(stderr, "[runahead] rollback load FAILED; disabling run-ahead "
                        "(the guest has advanced %d extra frame(s))\n", s_frames);
        s_frames = 0;
    }

    /* snes_frame_counter is host state and is not in the snapshot, so it does
     * not rewind on its own. Left alone it would advance N+1 per displayed
     * frame and every frame-numbered instrument -- the perf log, the OSD, the
     * frame-keyed traps -- would read N+1x fast. */
    snes_frame_counter = saved_counter;
    g_snes->disableRender = saved_render;
    return 1;
}
