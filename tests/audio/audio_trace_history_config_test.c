#include <stdio.h>

#include "audio_trace.h"

#ifndef EXPECT_AUDIO_TRACE_HISTORY
#error "EXPECT_AUDIO_TRACE_HISTORY must be defined"
#endif

static int failures;

static void check_int(const char *what, int got, int want) {
  if (got != want) {
    fprintf(stderr, "%s: got %d want %d\n", what, got, want);
    failures++;
  }
}

int main(void) {
  check_int("SNESRECOMP_AUDIO_TRACE_HISTORY",
            SNESRECOMP_AUDIO_TRACE_HISTORY,
            EXPECT_AUDIO_TRACE_HISTORY);

#if EXPECT_AUDIO_TRACE_HISTORY == 0
  check_int("AUDIO_TRACE_PCM_RING", AUDIO_TRACE_PCM_RING, 0);
  check_int("AUDIO_TRACE_EVENT_RING", AUDIO_TRACE_EVENT_RING, 0);
  check_int("AUDIO_TRACE_SNAP_RING", AUDIO_TRACE_SNAP_RING, 0);
  check_int("AUDIO_TRACE_HAS_STORAGE", AUDIO_TRACE_HAS_STORAGE, 0);
  check_int("AUDIO_TRACE_WRITES_HISTORY", AUDIO_TRACE_WRITES_HISTORY, 0);
#elif EXPECT_AUDIO_TRACE_HISTORY == 1
  check_int("AUDIO_TRACE_PCM_RING", AUDIO_TRACE_PCM_RING, 1u << 16);
  check_int("AUDIO_TRACE_EVENT_RING", AUDIO_TRACE_EVENT_RING, 1u << 14);
  check_int("AUDIO_TRACE_SNAP_RING", AUDIO_TRACE_SNAP_RING, 1u << 9);
  check_int("AUDIO_TRACE_HAS_STORAGE", AUDIO_TRACE_HAS_STORAGE, 1);
  check_int("AUDIO_TRACE_WRITES_HISTORY", AUDIO_TRACE_WRITES_HISTORY, 1);
#elif EXPECT_AUDIO_TRACE_HISTORY == 2
  check_int("AUDIO_TRACE_PCM_RING", AUDIO_TRACE_PCM_RING, 1u << 22);
  check_int("AUDIO_TRACE_EVENT_RING", AUDIO_TRACE_EVENT_RING, 1u << 19);
  check_int("AUDIO_TRACE_SNAP_RING", AUDIO_TRACE_SNAP_RING, 1u << 12);
  check_int("AUDIO_TRACE_HAS_STORAGE", AUDIO_TRACE_HAS_STORAGE, 1);
  check_int("AUDIO_TRACE_WRITES_HISTORY", AUDIO_TRACE_WRITES_HISTORY, 1);
#elif EXPECT_AUDIO_TRACE_HISTORY == 3
  check_int("AUDIO_TRACE_PCM_RING", AUDIO_TRACE_PCM_RING, 1u << 22);
  check_int("AUDIO_TRACE_EVENT_RING", AUDIO_TRACE_EVENT_RING, 1u << 19);
  check_int("AUDIO_TRACE_SNAP_RING", AUDIO_TRACE_SNAP_RING, 1u << 12);
  check_int("AUDIO_TRACE_HAS_STORAGE", AUDIO_TRACE_HAS_STORAGE, 1);
  check_int("AUDIO_TRACE_WRITES_HISTORY", AUDIO_TRACE_WRITES_HISTORY, 0);
#else
#error "unsupported EXPECT_AUDIO_TRACE_HISTORY"
#endif

  if (failures) return 1;
  printf("audio_trace_history_config_test: mode=%d\n",
         SNESRECOMP_AUDIO_TRACE_HISTORY);
  return 0;
}
