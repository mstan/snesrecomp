/* Dispatch tables for a SETUP HOST: a build with no recompiled code.
 *
 * The runner links against g_dispatch_table / g_ram_routine_guards, which the
 * recompiler normally emits into src/gen/dispatch_v2.c. A setup host has no
 * src/gen -- that is its definition -- so this file provides the two symbols
 * with nothing in them.
 *
 * This is NOT a stub in the sense recomp-ai-rules forbids: no guest code is
 * ever executed against these tables. SnesInit() refuses to boot a guest at
 * all when SNESRECOMP_SETUP_HOST is defined (see common_cpu_infra.c), so the
 * only reachable path in this binary is the launcher's Generate & rebuild
 * wizard, which replaces the binary with a real one. The tables exist so the
 * link resolves; an empty table means every lookup misses, and a miss here
 * would be a bug in the guard, not a fallback.
 *
 * Both tables mirror what program_emit.py produces for an empty game so that
 * nothing in cpu_state.c has to special-case this build: the dispatch table
 * reports zero rows, and the guard table carries the same single sentinel row
 * the emitter writes when a cfg declares no ram_routine. */

#include "cpu_state.h"

#if !defined(SNESRECOMP_SETUP_HOST)
#error "setup_host_dispatch.c is only compiled into SNESRECOMP_SETUP_HOST builds"
#endif

/* Zero known function boundaries: the binary search in cpu_dispatch_pc()
 * runs over [0, 0) and reports a miss. One sentinel row keeps the array
 * non-empty, which C requires. */
const DispatchEntry g_dispatch_table[] = {
    { 0xFFFFFFFFu, { NULL, NULL, NULL, NULL }, 0 },
};
const unsigned g_dispatch_table_count = 0u;

const RamRoutineGuard g_ram_routine_guards[] = {
    { 0xFFFFFFFFu, 0u, 0u },
};
const unsigned g_ram_routine_guard_count =
    (unsigned)(sizeof(g_ram_routine_guards) / sizeof(g_ram_routine_guards[0]));
