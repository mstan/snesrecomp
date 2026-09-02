/*
 * cpu_pc24_resumable() — the predicate that decides whether a host may resume
 * guest execution at a saved cursor.
 *
 *   cc -I runner/src -I runner/src/snes tests/cpu/pc24_resumable_test.c \
 *      -o /tmp/pc24_resumable_test && /tmp/pc24_resumable_test
 *
 * Why this is pinned rather than eyeballed: the cursor is host state that no
 * guest snapshot carries, so a state load or a rollback restore that misses it
 * leaves the host resuming on whatever happened to be in the variable. An
 * unrestored resume PC is what wedged the Windows guest at reset PPU state.
 * The predicate is the only thing between that and executing data.
 *
 * It is deliberately permissive: it rejects only what CANNOT be code. A false
 * "not resumable" costs a needless recovery to the last-good cursor; a false
 * "resumable" executes whatever lives there. So every case below that expects
 * 0 is a real hazard, and the ones expecting 1 exist to stop the predicate
 * drifting into rejecting legitimate ROM, SRAM and WRAM cursors.
 */

#include <stdio.h>

/* The REAL header, not a copy. A test that restates the predicate would keep
 * passing after the predicate changed, which is the one thing it must not do
 * (workspace rules: a leaf that re-implements a rule cannot inherit fixes to
 * it). cpu_state.h compiles standalone against runner/src. */
#include "cpu_state.h"

static int fails;

static void expect(uint32_t pc, int want, const char *why) {
    int got = cpu_pc24_resumable(pc);
    if (got != want) {
        printf("FAIL $%06X -> %d, expected %d — %s\n",
               (unsigned)pc, got, want, why);
        fails++;
    } else {
        printf("  ok  $%06X -> %d  %s\n", (unsigned)pc, got, why);
    }
}

int main(void) {
    /* The sentinel. Zero is "never booted", not bank 0 offset 0. */
    expect(0x000000, 0, "null cursor is the never-booted sentinel");

    /* WRAM proper and its low-bank mirrors are legitimate execution targets:
     * this game runs copied-to-RAM routines. */
    expect(0x7E0000, 1, "WRAM bank 7E, start");
    expect(0x7E2100, 1, "WRAM 7E — the MMIO rule must not reach bank 7E");
    expect(0x7FFFFF, 1, "WRAM bank 7F, end");
    expect(0x000100, 1, "low-bank WRAM mirror");
    expect(0x001FFF, 1, "last byte below the MMIO window");
    expect(0xBF1FFF, 1, "mirror in the upper mirror banks");

    /* MMIO is readable and is not code. Resuming here executes register
     * side effects as instructions. */
    expect(0x002100, 0, "PPU registers");
    expect(0x004200, 0, "CPU/DMA registers");
    expect(0x3F5FFF, 0, "last byte of the MMIO window, low mirror");
    expect(0x802100, 0, "MMIO through the fast mirror");
    expect(0xBF4200, 0, "MMIO at the top of the mirror range");
    expect(0x006000, 1, "0x6000 is past MMIO — SRAM window, not rejected");

    /* Banks outside the mirror ranges have no MMIO window at all. */
    expect(0x402100, 1, "bank 40 has no MMIO window");
    expect(0xC02100, 1, "HiROM bank C0 has no MMIO window");

    /* The vector table is data. Resuming there executes the vectors. */
    expect(0x00FFE0, 0, "first vector-table byte");
    expect(0x00FFFF, 0, "last vector-table byte");
    expect(0x80FFE0, 0, "vector table through the fast mirror");
    expect(0x00FFDF, 1, "one byte below the vectors is still ROM");
    expect(0xC0FFE0, 1, "HiROM bank C0 has no vector table at FFE0");

    /* Ordinary code. */
    expect(0x008000, 1, "LoROM reset area");
    expect(0xC00000, 1, "HiROM code");

    if (fails) {
        printf("\n%d case(s) failed\n", fails);
        return 1;
    }
    printf("\nall cases passed\n");
    return 0;
}
