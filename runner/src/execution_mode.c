#include "execution_mode.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int equals_ignore_case(const char *a, const char *b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
      return 0;
  }
  return *a == '\0' && *b == '\0';
}

static const struct {
  const char *name;
  SnesrecompExecutionMode mode;
} kModeNames[] = {
    {"off",    SNESRECOMP_EXECUTION_MODE_OFF},
    {"on",     SNESRECOMP_EXECUTION_MODE_ON},
    {"force",  SNESRECOMP_EXECUTION_MODE_FORCE},
    {"verify", SNESRECOMP_EXECUTION_MODE_VERIFY},
    {"auto",   SNESRECOMP_EXECUTION_MODE_AUTO},
    /* Legacy spellings — the two-value vocabulary this file shipped with. */
    {"lle",    SNESRECOMP_EXECUTION_MODE_OFF},
    {"hle",    SNESRECOMP_EXECUTION_MODE_ON},
};

const char *snesrecomp_execution_mode_name(SnesrecompExecutionMode mode) {
  switch (mode) {
    case SNESRECOMP_EXECUTION_MODE_OFF:    return "off";
    case SNESRECOMP_EXECUTION_MODE_ON:     return "on";
    case SNESRECOMP_EXECUTION_MODE_FORCE:  return "force";
    case SNESRECOMP_EXECUTION_MODE_VERIFY: return "verify";
    case SNESRECOMP_EXECUTION_MODE_AUTO:   return "auto";
  }
  return "off";
}

bool snesrecomp_execution_optimized_allowed(SnesrecompExecutionMode mode) {
  /* VERIFY runs the replacement — it just does not publish its result. */
  return mode != SNESRECOMP_EXECUTION_MODE_OFF;
}

bool snesrecomp_execution_publishes_floor(SnesrecompExecutionMode mode) {
  return mode == SNESRECOMP_EXECUTION_MODE_OFF ||
         mode == SNESRECOMP_EXECUTION_MODE_VERIFY;
}

bool snesrecomp_execution_fallback_is_failure(SnesrecompExecutionMode mode) {
  return mode == SNESRECOMP_EXECUTION_MODE_FORCE;
}

/* Registered optimized replacements. Names are borrowed, not copied — call
 * sites pass string literals. */
enum { kMaxOptimizations = 32 };
static const char *s_optimizations[kMaxOptimizations];
static unsigned s_optimization_count;

void snesrecomp_execution_register_optimization(const char *name) {
  if (!name || !name[0])
    return;
  if (s_optimization_count < kMaxOptimizations)
    s_optimizations[s_optimization_count] = name;
  s_optimization_count++;
  fprintf(stderr, "[execution_mode] registered optimization: %s\n", name);
}

unsigned snesrecomp_execution_optimization_count(void) {
  return s_optimization_count;
}

static unsigned s_fallbacks;

unsigned snesrecomp_execution_note_fallback(const char *what) {
  s_fallbacks++;
  /* Loud, but not unbounded: the first few name themselves, then the running
   * total is what matters. A miss storm must not become the log. */
  if (s_fallbacks <= 8u || (s_fallbacks % 256u) == 0u) {
    fprintf(stderr, "[execution_mode] FALLBACK to floor: %s (total %u)\n",
            what ? what : "(unnamed)", s_fallbacks);
  }
  return s_fallbacks;
}

unsigned snesrecomp_execution_fallback_count(void) { return s_fallbacks; }

SnesrecompExecutionMode snesrecomp_execution_mode(
    SnesrecompExecutionMode default_mode) {
  static int resolved = 0;
  static SnesrecompExecutionMode mode;
  if (resolved)
    return mode;

  mode = default_mode;
  const char *source = "build default";

  const char *value = getenv("SNESRECOMP_EXECUTION_MODE");
  if (value && value[0]) {
    size_t i;
    int matched = 0;
    for (i = 0; i < sizeof(kModeNames) / sizeof(kModeNames[0]); i++) {
      if (equals_ignore_case(value, kModeNames[i].name)) {
        mode = kModeNames[i].mode;
        source = "SNESRECOMP_EXECUTION_MODE";
        matched = 1;
        break;
      }
    }
    if (!matched) {
      fprintf(stderr,
              "[execution_mode] ignoring SNESRECOMP_EXECUTION_MODE='%s' "
              "(expected off|on|force|verify|auto)\n",
              value);
    }
  }

  /* Master force-floor last, so it overrides every other selection. Its whole
   * purpose is that a bug report can be reproduced on the faithful path
   * without knowing which optimization is implicated. */
  const char *floor_env = getenv("SNESRECOMP_FORCE_FLOOR");
  if (floor_env && floor_env[0] && floor_env[0] != '0') {
    mode = SNESRECOMP_EXECUTION_MODE_OFF;
    source = "SNESRECOMP_FORCE_FLOOR";
  }

  fprintf(stderr,
          "[execution_mode] policy=%s (default %s, from %s) — "
          "optimized=%s publishes=%s registered=%u\n",
          snesrecomp_execution_mode_name(mode),
          snesrecomp_execution_mode_name(default_mode), source,
          snesrecomp_execution_optimized_allowed(mode) ? "allowed" : "blocked",
          snesrecomp_execution_publishes_floor(mode) ? "floor" : "optimized",
          s_optimization_count);
  if (s_optimization_count == 0u) {
    /* Do not let a policy word imply behavior the build cannot deliver. */
    fprintf(stderr,
            "[execution_mode] no optimized replacements are registered in this "
            "build — every policy runs the faithful floor.\n");
  } else {
    unsigned i;
    const unsigned shown = s_optimization_count < kMaxOptimizations
                               ? s_optimization_count
                               : kMaxOptimizations;
    for (i = 0; i < shown; i++)
      fprintf(stderr, "[execution_mode]   optimization[%u]=%s\n", i,
              s_optimizations[i]);
  }

  resolved = 1;
  return mode;
}
