/*
 * harness_glue.c -- headless replacements for the SDL/main.c-provided glue the
 * runner references, for the co-simulation A-side (SNES_COSIM.md). Built only
 * with the harness (dev/diagnostics).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "types.h"   /* NORETURN, Die decl */

/* APU lock: guards the APU state vs the SDL audio-callback thread. The harness
 * opens NO audio device, so that thread never exists — a no-op lock is correct
 * (single-threaded), and keeps Gate-1 determinism (no host threads). */
void RtlApuLock(void)   {}
void RtlApuUnlock(void) {}

/* Debug-server WRAM write hooks. Non-inline decls in debug_server.h are only
 * defined by debug_server.c (compiled at SNESRECOMP_TRACE=1). We build with
 * TRACE=0 and our own cosim server, so no-op them. */
void debug_on_wram_write_byte(uint32_t addr, uint8_t old_val, uint8_t new_val) {
    (void)addr; (void)old_val; (void)new_val;
}
void debug_on_wram_write_word(uint32_t addr, uint16_t old_val, uint16_t new_val) {
    (void)addr; (void)old_val; (void)new_val;
}

/* PPU/widescreen globals normally owned by main.c. Headless = authentic 4:3,
 * new-PPU path (same defaults main.c uses). */
bool g_new_ppu = true;
int  g_ws_extra = 0;

/* main.c normally shows an SDL message box; headless just prints + exits. */
void NORETURN Die(const char *error) {
    fprintf(stderr, "cosim-harness FATAL: %s\n", error ? error : "(null)");
    exit(1);
}
