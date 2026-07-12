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

const char *snesrecomp_execution_mode_name(SnesrecompExecutionMode mode) {
  return mode == SNESRECOMP_EXECUTION_MODE_LLE ? "lle" : "hle";
}

SnesrecompExecutionMode snesrecomp_execution_mode(
    SnesrecompExecutionMode default_mode) {
  static int resolved = 0;
  static SnesrecompExecutionMode mode;
  if (resolved)
    return mode;

  mode = default_mode;
  const char *value = getenv("SNESRECOMP_EXECUTION_MODE");
  if (value && value[0]) {
    if (equals_ignore_case(value, "hle"))
      mode = SNESRECOMP_EXECUTION_MODE_HLE;
    else if (equals_ignore_case(value, "lle"))
      mode = SNESRECOMP_EXECUTION_MODE_LLE;
    else
      fprintf(stderr,
              "[execution_mode] ignoring SNESRECOMP_EXECUTION_MODE='%s' "
              "(expected hle or lle)\n",
              value);
  }
  fprintf(stderr, "[execution_mode] selected %s (default %s)\n",
          snesrecomp_execution_mode_name(mode),
          snesrecomp_execution_mode_name(default_mode));
  resolved = 1;
  return mode;
}
