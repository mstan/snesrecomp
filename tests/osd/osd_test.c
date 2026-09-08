/*
 * osd_test.c — snes_osd.c's composition and image path, with no window.
 *
 * The OSD is host chrome, so the thing worth regressing is not how it looks
 * but what it CLAIMS: that the status line says what fps and turbo are, that a
 * toast expires, and that the image is only produced when something is
 * actually visible. A port that draws an overlay every frame because
 * needs_present() never goes false is a real bug this catches.
 */

#include "snes_osd.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(void) {
    /* SDL_GetTicks / SDL_GetPerformanceCounter need the timer subsystem; no
     * video, so this runs headless in CI. */
    if (!SDL_Init(SDL_INIT_TIMER)) {
        /* SDL2 returns 0 on success, SDL3 returns true — accept either. */
        if (SDL_Init(SDL_INIT_TIMER) != 0 && SDL_WasInit(SDL_INIT_TIMER) == 0) {
            printf("SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    const uint32_t *px = NULL;
    int w = 0, h = 0;

    /* Nothing on screen: no image, and nothing to present. */
    check(!snes_osd_image(&px, &w, &h), "idle OSD produces no image");
    check(!snes_osd_needs_present(), "and asks for no present");
    check(!snes_osd_fps_visible(), "FPS readout starts hidden");

    /* FPS readout on. */
    snes_osd_toggle_fps();
    check(snes_osd_fps_visible(), "toggle shows the FPS readout");
    check(snes_osd_needs_present(), "which needs a present");
    check(snes_osd_image(&px, &w, &h) && px && w > 0 && h > 0,
          "and produces a non-empty image");

    /* The readout must be right EARLY, not after a second of climbing.
     *
     * The rolling mean divides by the full 64-slot window, so before the
     * window has filled the raw accumulator is diluted by the zeroed slots --
     * which is exactly why the overlay used to count up from 0 to 60 over its
     * first second. Ten frames at ~10ms is ~100fps; a diluted value would read
     * around 10/64 of that. */
    for (int i = 0; i < 10; i++) { snes_osd_note_frame(); SDL_Delay(10); }
    {
        const float early = snes_osd_fps();
        printf("  ..  after 10 frames: %.1f fps (raw window would show ~%.1f)\n",
               (double)early, (double)early * 10.0 / 64.0);
        check(early > 40.0f,
              "the average is window-corrected, so it does not ramp from zero");
    }

    /* Feed frames at a known cadence and confirm the average lands near it.
     * Deliberately loose: SDL_Delay is not precise, and asserting an exact
     * fps here would be a test of the host's scheduler, not of the OSD. */
    for (int i = 0; i < 70; i++) {
        snes_osd_note_frame();
        SDL_Delay(10);          /* ~100 fps */
    }
    const float fps = snes_osd_fps();
    printf("  ..  measured %.1f fps over 70 frames at ~10ms\n", (double)fps);
    check(fps > 40.0f && fps < 200.0f, "rolling average is in a sane band");

    snes_osd_toggle_fps();
    check(!snes_osd_fps_visible(), "toggle hides it again");

    /* Turbo alone still shows a status line — a port with turbo but no FPS
     * readout must still get the indicator. */
    snes_osd_set_turbo(1);
    check(snes_osd_image(&px, &w, &h), "turbo alone produces a status line");
    snes_osd_set_turbo(0);

    /* A toast appears and then expires on its own. */
    snes_osd_push_slot_saved(3);
    check(snes_osd_image(&px, &w, &h), "a slot toast is visible immediately");
    SDL_Delay(60);
    check(snes_osd_image(&px, &w, &h), "and still visible well before expiry");

    snes_osd_push("brief", 30);
    SDL_Delay(120);
    check(!snes_osd_image(&px, &w, &h), "a short toast expires by itself");
    check(snes_osd_needs_present(),
          "and asks for one more present so the host can clear it");
    snes_osd_present_done();
    check(!snes_osd_needs_present(), "which present_done() then satisfies");

    printf(failures ? "FAILED\n" : "PASSED\n");
    return failures;
}
