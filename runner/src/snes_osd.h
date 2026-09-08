#ifndef SNES_OSD_H
#define SNES_OSD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host on-screen display — FPS readout, turbo indicator, and timed toasts.
 *
 * Ported from psxrecomp's runtime/src/host_osd.c (the 8x8 font and the
 * toast/expiry state machine are that file's), with the same division of
 * labour snes_savestate_menu.c already established: the framework owns the
 * state and the pixels, and the host only pumps events and presents.
 *
 * The reason it is here and not in a game's main.c: Metal Warriors grew its
 * own FPS readout as ~40 lines of digit blitter in src/main.c, and the SNES
 * side has ~25 port repos generated from one scaffold. A second port copying
 * that would be the 25-copies problem snes_savestate_menu.c's header already
 * argues against. So the composition rule — what the status line says, how
 * long a toast lives — lives once, here.
 *
 * NOTHING here touches guest state. The OSD is not in VRAM, not in a
 * savestate, and not in the netplay-visible frame: it is composited over the
 * presented image after the PPU is done. A peer that draws a toast and one
 * that does not still agree about the emulated machine.
 *
 * A host wires it up in four places:
 *
 *   on the FPS hotkey (config.ini [KeyMap] DisplayPerf, default "F"):
 *       snes_osd_toggle_fps();
 *   when turbo changes:
 *       snes_osd_set_turbo(on);
 *   once per presented frame, after the PPU has composited:
 *       snes_osd_note_frame();
 *   at present time:
 *       snes_osd_draw_sdl(renderer);        (SDL_Renderer path)
 *     or snes_osd_image(&px,&w,&h) + snes_osd_present_done()  (blit it yourself)
 *
 * Toasts are for things that happened: "Slot 3 saved", "Slot 3 loaded".
 */

/* ---- FPS readout ------------------------------------------------------- */

/* Toggle the persistent FPS readout. Bound to [KeyMap] DisplayPerf. */
void snes_osd_toggle_fps(void);
int  snes_osd_fps_visible(void);
void snes_osd_set_fps_visible(int on);

/*
 * Call once per presented frame. The OSD times the interval itself and keeps
 * a 64-frame rolling average, so every port reports FPS the same way instead
 * of each one inventing its own smoothing.
 *
 * Measures PRESENT-to-PRESENT wall time, which is the number a player means
 * by "fps". It is not emulation cost per frame — under a frame limiter this
 * pins to the refresh rate and says nothing about headroom. For headroom, use
 * the debug server, not this.
 */
void snes_osd_note_frame(void);

/* Last computed rolling average, or 0 before enough frames have elapsed. */
float snes_osd_fps(void);

/* ---- turbo indicator ---------------------------------------------------- */

/* Show/hide "TURBO" beside the FPS readout. Cheap to call every frame. */
void snes_osd_set_turbo(int on);
int  snes_osd_turbo(void);

/* ---- toasts ------------------------------------------------------------- */

/* Timed top-left message. duration_ms <= 0 uses the 2000ms default. */
void snes_osd_push(const char *msg, int duration_ms);

/* Convenience for the save-state paths: "Slot N saved" / "Slot N loaded" /
 * "Slot N empty", so every port words them identically. */
void snes_osd_push_slot_saved(int slot);
void snes_osd_push_slot_loaded(int slot);
void snes_osd_push_slot_empty(int slot);

/* ---- presenting --------------------------------------------------------- */

/*
 * 1 while anything is visible, or while one more present is needed to clear
 * what just expired. A host that only redraws on change can gate on this.
 */
int snes_osd_needs_present(void);

/*
 * ARGB8888 image for whatever is currently showing (status line and/or
 * toast). Valid until the next snes_osd_* call. Returns 1 when there are
 * pixels. Call snes_osd_present_done() after compositing.
 */
int  snes_osd_image(const uint32_t **pixels, int *w, int *h);
void snes_osd_present_done(void);

/* SDL_Renderer path: draw the overlay over the current backbuffer. */
struct SDL_Renderer;
void snes_osd_draw_sdl(struct SDL_Renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif /* SNES_OSD_H */
