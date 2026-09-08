/*
 * rewind_test.c — the rewind ring's bookkeeping, with no machine.
 *
 * RtlSaveSnapshotToMemory / RtlLoadSnapshotFromMemory are stubbed to write and
 * read a recognisable frame number, so this tests the part that is actually
 * new -- ordering, clamping, and what commit() throws away -- rather than
 * re-testing the save format, which the save-state menu already exercises.
 *
 * The ordering question is the one worth pinning down: slot 0 must be the
 * NEWEST snapshot and the index must grow going back in time, because every
 * caller and the filmstrip's left-to-right layout depend on it.
 */

#include "snes_rewind.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static int failures;
static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

/* ---- stubbed machine ---------------------------------------------------- */

static uint32_t g_fake_frame;      /* "the machine": one number */
static uint32_t g_restored;        /* what the last load put back */
static int      g_refuse_load;

size_t RtlSaveSnapshotToMemory(void *data, size_t capacity) {
    if (capacity < sizeof(uint32_t) * 2) return 0;
    ((uint32_t *)data)[0] = 0x5245574eu;      /* 'REWN' */
    ((uint32_t *)data)[1] = g_fake_frame;
    return sizeof(uint32_t) * 2;
}

int RtlLoadSnapshotFromMemory(const void *data, size_t size) {
    if (g_refuse_load) return 0;
    if (size < sizeof(uint32_t) * 2) return 0;
    if (((const uint32_t *)data)[0] != 0x5245574eu) return 0;
    g_restored = ((const uint32_t *)data)[1];
    g_fake_frame = g_restored;
    return 1;
}

int snes_netplay_active(void) { return 0; }

/* ---- helpers ------------------------------------------------------------ */

/* interval is 1 in these tests, so one call == one snapshot. */
static void advance(int frames) {
    int i;
    for (i = 0; i < frames; i++) {
        g_fake_frame++;
        snes_rewind_note_frame();
    }
}

int main(void) {
    setenv("SNESRECOMP_REWIND_DEPTH", "8", 1);
    setenv("SNESRECOMP_REWIND_INTERVAL", "1", 1);
    snes_rewind_configure();
    check(snes_rewind_enabled(), "the ring configures and is enabled");

    check(snes_rewind_open() == 0,
          "open refuses with no history — there is nothing to go back to");

    advance(5);                       /* snapshots of frames 1..5 */
    check(snes_rewind_open() == 1, "open succeeds once there is history");
    check(snes_rewind_is_open(), "and reports itself open");

    /* Selection 0 is the newest. Stepping back must walk backwards in time. */
    snes_rewind_step(-1);
    snes_rewind_step(-1);
    snes_rewind_commit();
    check(g_restored == 3,
          "sel 0 is the newest, and two steps back lands two snapshots earlier");
    check(!snes_rewind_is_open(), "commit closes the overlay");

    /* Everything after the landing point is gone: a second rewind must not be
     * able to travel to a future that no longer happens. */
    check(snes_rewind_open() == 1, "history before the landing point survives");
    snes_rewind_step(+1);             /* try to go forwards from the newest */
    snes_rewind_commit();
    check(g_restored == 3,
          "the abandoned timeline is dropped, so forwards from newest is a no-op");

    /* Clamping, not wrapping. */
    g_fake_frame = 100;
    advance(6);
    check(snes_rewind_open() == 1, "reopens after more play");
    {
        int i;
        for (i = 0; i < 50; i++) snes_rewind_step(-1);   /* way past the end */
    }
    snes_rewind_commit();
    check(g_restored != 0 && g_restored <= 106,
          "stepping past the oldest clamps instead of wrapping to the newest");

    /* A refused load must leave the machine alone. */
    g_fake_frame = 500;
    advance(4);
    {
        const uint32_t before = g_fake_frame;
        g_refuse_load = 1;
        snes_rewind_open();
        snes_rewind_step(-1);
        snes_rewind_commit();
        g_refuse_load = 0;
        check(g_fake_frame == before,
              "a refused snapshot leaves the machine where it was");
    }

    /* Cancel changes nothing. */
    {
        const uint32_t before = g_fake_frame;
        snes_rewind_open();
        snes_rewind_step(-1);
        snes_rewind_close();
        check(g_fake_frame == before, "closing without commit does not move the machine");
        check(!snes_rewind_is_open(), "and leaves the overlay closed");
    }

    snes_rewind_shutdown();
    printf(failures ? "FAILED\n" : "PASSED\n");
    return failures;
}
