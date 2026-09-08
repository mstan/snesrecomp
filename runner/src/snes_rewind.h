#ifndef SNES_REWIND_H
#define SNES_REWIND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Local rewind — a ring of whole-machine snapshots and a filmstrip to pick
 * one out of.
 *
 * Same division of labour as snes_savestate_menu.c: the framework owns the
 * ring, the selection and the pixels; the host only pumps events and presents.
 * psxrecomp's psx_rewind.c is the model, but not the source — that one is
 * built on retcomm-rbengine's snap_ring and PSX boot-state blobs, neither of
 * which exists here. What SNES has instead is snes_saveload() over a caller
 * supplied sink, so a snapshot is just that sink pointed at memory.
 *
 * NEVER runs during netplay. Rewinding is one machine moving its own clock
 * backwards; two peers cannot do that independently and still agree, and the
 * workspace rule against stepping one observer to sync it with another is
 * exactly this case. snes_rewind_note_frame() captures nothing while
 * snes_netplay_active(), and open() refuses.
 *
 * Cost is honest about itself: a snapshot is the whole machine (~256 KB), and
 * the ring is depth x that. Defaults to 60 snapshots every 6 frames — six
 * seconds of history for ~16 MB. Env overrides:
 *   SNESRECOMP_REWIND=0        disable entirely
 *   SNESRECOMP_REWIND_DEPTH    snapshots kept (4..240, default 60)
 *   SNESRECOMP_REWIND_INTERVAL guest frames between captures (1..60, default 6)
 *
 * Host wiring:
 *   once, after the machine exists:      snes_rewind_configure()
 *   per guest frame, after RtlRunFrame:  snes_rewind_note_frame()
 *   per present, after compositing:      snes_rewind_note_framebuffer(fb,w,h)
 *   on the hotkey / pad gesture:         snes_rewind_open()
 *   while open, a modal pump calling:    snes_rewind_step(-1|+1)
 *                                        snes_rewind_commit() / _close()
 *                                        snes_rewind_overlay_image(...)
 */

/* Allocate the ring and read the env overrides. Safe to call more than once;
 * a second call with the same settings is a no-op. */
void snes_rewind_configure(void);
void snes_rewind_shutdown(void);

/* False when disabled by env, or when the ring could not be allocated. */
int snes_rewind_enabled(void);

/* One emulated frame elapsed. Captures when the interval comes round, and
 * only when the machine is actually running (not while the rewind UI or the
 * save-state menu is up, and never during netplay). */
void snes_rewind_note_frame(void);

/* Offer the presented frame as the thumbnail for the NEXT capture. Cheap: it
 * downsamples into a small fixed buffer and keeps nothing else. */
void snes_rewind_note_framebuffer(const uint32_t *fb, int width, int height);

/* Enter rewind. Returns 0 (and does nothing) when disabled, during netplay,
 * or when fewer than two snapshots exist — there is nothing to go back to. */
int snes_rewind_open(void);
int snes_rewind_is_open(void);

/* Move the selection. -1 is further back in time, +1 is nearer the present.
 * Clamps at both ends rather than wrapping: wrapping from the oldest snapshot
 * to the newest would silently jump the player forwards. */
void snes_rewind_step(int dir);

/* Restore the selected snapshot and close. The snapshots after it are
 * dropped: the machine is now at that point, and keeping the ones from the
 * timeline just abandoned would let a second rewind travel to a future that
 * no longer happened. */
void snes_rewind_commit(void);

/* Leave without changing the machine. */
void snes_rewind_close(void);

/* How far back the selection sits, in emulated seconds, for the caption. */
float snes_rewind_selected_seconds(void);

/* ARGB8888 filmstrip. Valid until the next snes_rewind_* call. */
int snes_rewind_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* SNES_REWIND_H */
