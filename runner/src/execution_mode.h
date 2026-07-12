#ifndef SNESRECOMP_EXECUTION_MODE_H
#define SNESRECOMP_EXECUTION_MODE_H

typedef enum SnesrecompExecutionMode {
  SNESRECOMP_EXECUTION_MODE_HLE = 0,
  SNESRECOMP_EXECUTION_MODE_LLE = 1,
} SnesrecompExecutionMode;

/* Resolve the process-wide CPU orchestration mode.  The shared override is
 * SNESRECOMP_EXECUTION_MODE=hle|lle; default_mode remains a per-game policy. */
SnesrecompExecutionMode snesrecomp_execution_mode(
    SnesrecompExecutionMode default_mode);
const char *snesrecomp_execution_mode_name(SnesrecompExecutionMode mode);

#endif
