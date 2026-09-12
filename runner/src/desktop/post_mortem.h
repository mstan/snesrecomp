#pragma once

/* Shared unified exit/crash diagnostic dump.
 *
 * Single public entry. See post_mortem.c for what gets dumped and
 * design rationale. Output: build/last_run_report.json (overwritten).
 *
 * `reason` is a short tag ("seh" / "signal" / "atexit" / "on_demand").
 * `fault_info` is a Windows EXCEPTION_POINTERS* (cast to void* so the
 * header doesn't drag in windows.h); pass NULL outside the SEH path.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

void recomp_post_mortem_dump(const char *reason, void *fault_info);

/* Per-game report section.
 *
 * A port with game-specific state worth capturing on a crash (a game-mode
 * byte, an object table, the slot that was being drawn) registers a writer
 * here. Without this the only way to add one was to fork post_mortem.c, and
 * a fork stops inheriting every later fix to the generic dumps — which is
 * exactly what happened to SuperMetroidRecomp.
 *
 * The callback emits complete top-level JSON members, trailing comma
 * included:
 *
 *     static void sm_section(FILE *f) {
 *         fprintf(f, "  \"sm\": {\"game_state\": %u},\n", state);
 *     }
 *     recomp_post_mortem_set_game_section(&sm_section);
 *
 * It runs inside the dump lock from a crash handler, so it must not
 * allocate, take locks, or call back into the guest. Pass NULL to clear. */
typedef void (*RecompPostMortemGameSectionFn)(FILE *f);
void recomp_post_mortem_set_game_section(RecompPostMortemGameSectionFn fn);

#ifdef __cplusplus
}
#endif
