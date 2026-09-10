#ifndef SNES_RUNAHEAD_H
#define SNES_RUNAHEAD_H

#include <stdint.h>

/*
 * Run-ahead: hide N frames of the game's own input latency.
 *
 * Every frame, the emulator advances once for real, then speculatively runs N
 * more with the same input and shows the LAST one. The guest only ever
 * advances one frame per host iteration -- the speculative run is rewound --
 * but the picture the player sees is the state N frames after their input was
 * read. Games that buffer input for a few frames before acting on it feel
 * that much more responsive; nothing about the emulation changes.
 *
 * OFFLINE ONLY, and refused during netplay rather than merely discouraged.
 * Run-ahead is one machine speculating about its own future. Two peers cannot
 * each do that and still agree on a shared timeline, and netplay already owns
 * the rollback machinery this borrows -- letting both drive it at once would
 * have them fighting over the same snapshot buffers and the same audio
 * producer cursor.
 *
 * Cost per displayed frame: N extra emulated frames, one rollback snapshot
 * save and one load. On this scaffold an emulated frame is ~1.5-3 ms and a
 * snapshot is a few hundred KB, so N=1 roughly doubles emulation time and
 * still leaves most of a 16.6 ms budget. N is deliberately capped low.
 *
 * Why the ROLLBACK snapshot pair and not the ordinary savestate one: an
 * ordinary load re-anchors host clocks that do not rewind (the beam anchor,
 * the APU catch-up accumulators) because a one-off load can get away with it.
 * Run-ahead rewinds sixty times a second, so those clocks must rewind too --
 * exactly the case RtlRollbackSaveToMemory/RtlRollbackLoadFromMemory exist
 * for. See the contract above them in common_rtl.h.
 */

/* Read SNESRECOMP_RUNAHEAD from the environment (0..SNES_RUNAHEAD_MAX). Safe
 * to call more than once. The host normally calls snes_runahead_set_frames()
 * from its own config instead; the env override wins for a one-off test. */
void snes_runahead_configure(void);

#define SNES_RUNAHEAD_MAX 4

/* 0 disables. Values outside 0..SNES_RUNAHEAD_MAX are clamped. */
void snes_runahead_set_frames(int frames);
int  snes_runahead_frames(void);

/* True when run-ahead is on AND currently usable (offline, machine ready). */
int  snes_runahead_active(void);

/*
 * Advance the guest exactly one frame, speculating N ahead for the picture.
 *
 * Returns 1 when it handled the frame -- the caller must NOT also call
 * RtlRunFrame. Returns 0 when run-ahead is off or unavailable, and the caller
 * runs the frame itself as usual. A snapshot failure mid-way still returns 1:
 * the frame did advance, it just advanced without the speculation, which is a
 * dropped optimisation rather than a dropped frame.
 */
int snes_runahead_run_frame(uint32_t inputs);

/* Frees the snapshot buffer. */
void snes_runahead_shutdown(void);

#endif /* SNES_RUNAHEAD_H */
