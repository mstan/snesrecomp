#ifndef SNESRECOMP_EXECUTION_MODE_H
#define SNESRECOMP_EXECUTION_MODE_H

#include <stdbool.h>

/*
 * Execution policy for optimized (HLE) replacements layered over the faithful
 * LLE floor.
 *
 * The vocabulary is fixed by recomp-ai-rules/OPTIMIZATION.md §2 — every
 * optimization surface across the recomp ecosystem uses these same five
 * values, so a per-project spelling is a bug. Two rules from that section are
 * implemented here rather than left to each caller:
 *
 *   - "Startup output must print the effective policy."
 *   - "Keep a master force-floor control even after individual optimizations
 *      become default-on."
 *
 * PRINCIPLES.md ("LLE Is the Faithful Floor") is what makes the floor
 * mandatory in the SHIPPED binary, not merely in a dev build: the faithful
 * mode "may cease to be the normal play path after a replacement is promoted,
 * but it remains buildable, forceable, and authoritative." A compile-time-only
 * split cannot satisfy that — a release binary with the floor compiled out has
 * nothing to fall back to on a miss and nothing to force when arbitrating a
 * player's bug report. Hence a runtime policy; the build only picks its
 * default (SNESRECOMP_EXECUTION_DEFAULT).
 */
typedef enum SnesrecompExecutionMode {
  /* Force the faithful floor. No promoted replacement runs. */
  SNESRECOMP_EXECUTION_MODE_OFF = 0,
  /* Promoted fast paths run; an unsupported case falls back loudly. */
  SNESRECOMP_EXECUTION_MODE_ON = 1,
  /* The candidate must handle its configured scope — a fallback is a test
   * failure, not a recovery. For gate runs, never for shipping. */
  SNESRECOMP_EXECUTION_MODE_FORCE = 2,
  /* Run optimized and faithful from the same input, publish the FAITHFUL
   * result, and count/stop on mismatch. */
  SNESRECOMP_EXECUTION_MODE_VERIFY = 3,
  /* Use a promoted backend when its host requirements are met, else report
   * why and use the floor. */
  SNESRECOMP_EXECUTION_MODE_AUTO = 4,
} SnesrecompExecutionMode;

/* Legacy spellings kept so existing callers and docs that say "lle"/"hle"
 * keep working: lle is the floor (off), hle is promoted-with-fallback (on). */
#define SNESRECOMP_EXECUTION_MODE_LLE SNESRECOMP_EXECUTION_MODE_OFF
#define SNESRECOMP_EXECUTION_MODE_HLE SNESRECOMP_EXECUTION_MODE_ON

/*
 * Resolve the process-wide execution policy, once.
 *
 * Precedence, highest first:
 *   1. SNESRECOMP_FORCE_FLOOR=1  — the master force-floor. Always wins, so a
 *      bug report can be reproduced on the faithful path without knowing
 *      which individual optimization is implicated.
 *   2. SNESRECOMP_EXECUTION_MODE=off|on|force|verify|auto  (lle|hle accepted)
 *   3. default_mode — per-game policy, normally from the build's
 *      SNESRECOMP_EXECUTION_DEFAULT.
 *
 * Prints the effective policy on first call. Unparseable values are reported
 * and ignored rather than silently treated as the default.
 */
SnesrecompExecutionMode snesrecomp_execution_mode(
    SnesrecompExecutionMode default_mode);

/*
 * The build's configured default, from -DSNESRECOMP_EXECUTION_DEFAULT
 * (runner.cmake). Ports should call this rather than hardcoding a default, so
 * "what does a release build default to" is answered by the build, in one
 * place, instead of by a literal in each game's frame loop.
 */
#ifndef SNESRECOMP_EXECUTION_DEFAULT_MODE
#define SNESRECOMP_EXECUTION_DEFAULT_MODE SNESRECOMP_EXECUTION_MODE_OFF
#endif
static inline SnesrecompExecutionMode snesrecomp_execution_policy(void) {
  return snesrecomp_execution_mode(SNESRECOMP_EXECUTION_DEFAULT_MODE);
}

/* Lowercase policy name ("off", "on", "force", "verify", "auto"). */
const char *snesrecomp_execution_mode_name(SnesrecompExecutionMode mode);

/* True when a promoted replacement may run at all. False for OFF — and note
 * VERIFY is true here: verify RUNS the replacement, it just does not publish
 * its result. Callers deciding "may I execute the fast path" want this. */
bool snesrecomp_execution_optimized_allowed(SnesrecompExecutionMode mode);

/* True when the faithful result is the one that must be published. OFF and
 * VERIFY both publish the floor. Callers deciding "whose answer ships" want
 * this, and it is the guard that keeps VERIFY from changing behavior. */
bool snesrecomp_execution_publishes_floor(SnesrecompExecutionMode mode);

/* True when a fallback to the floor is a failure rather than a recovery
 * (FORCE). Call sites report a counted diagnostic either way; this only
 * decides whether it is fatal to a gate run. */
bool snesrecomp_execution_fallback_is_failure(SnesrecompExecutionMode mode);

/*
 * Register an optimized replacement that exists in this build.
 *
 * Without this the policy switch can lie: with no promoted replacement
 * compiled in, SNESRECOMP_EXECUTION_MODE=on would print "on" and change
 * nothing, and a reader would conclude HLE ran. Registration lets the startup
 * banner state how many replacements are actually available, so "on with 0
 * registered" reads as the no-op it is.
 *
 * Call once per replacement during startup, BEFORE the first policy query.
 * `name` should be stable and greppable (e.g. "apu.brr_shadow").
 */
void snesrecomp_execution_register_optimization(const char *name);

/* How many replacements registered. 0 means every policy behaves as OFF. */
unsigned snesrecomp_execution_optimization_count(void);

/*
 * Record that a promoted path could not handle an input and the floor ran
 * instead. `what` names the subsystem/entry (stable, greppable). Never let a
 * miss be silent — PRINCIPLES.md "HLE Dispatch Is an Allowlist — Make Misses
 * Loud". Returns the running total for that call site's convenience.
 */
unsigned snesrecomp_execution_note_fallback(const char *what);

/* Total loud fallbacks recorded this process. 0 with mode ON is the steady
 * state worth asserting in a gate run. */
unsigned snesrecomp_execution_fallback_count(void);

#endif
