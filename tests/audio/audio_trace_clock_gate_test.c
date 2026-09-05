#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_trace.h"

int snes_frame_counter = 1234;

static uint64_t g_now_ms;
static uint64_t g_wall_calls;
static uint64_t g_fopen_calls;
static int g_force_stats_env;
static const char *g_stats_env_value;

uint64_t audio_trace_test_wall_ms(void) {
  g_wall_calls++;
  return g_now_ms;
}

FILE *audio_trace_test_fopen(const char *path, const char *mode) {
  g_fopen_calls++;
  if (getenv("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL"))
    return NULL;
  return fopen(path, mode);
}

char *audio_trace_test_getenv(const char *name) {
  if (strcmp(name, "SNESRECOMP_AUDIO_STATS") == 0 && g_force_stats_env)
    return (char *)g_stats_env_value;
  return getenv(name);
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static void fail(const char *msg) {
  fprintf(stderr, "audio_trace_clock_gate_test: %s\n", msg);
  exit(1);
}

static void expect_u64(const char *name, uint64_t got, uint64_t want) {
  if (got != want) {
    fprintf(stderr,
            "audio_trace_clock_gate_test: %s got %llu want %llu\n",
            name, (unsigned long long)got, (unsigned long long)want);
    exit(1);
  }
}

static void emit_sample_at(uint64_t ms) {
  g_now_ms = ms;
  audio_trace_on_sample(123, -123, 0, 4);
}

static void emit_cadence_samples(void) {
  emit_sample_at(0);
  emit_sample_at(999);
  emit_sample_at(1000);
  emit_sample_at(1999);
  emit_sample_at(2000);
}

static uint64_t expected_off_wall_calls(uint64_t samples) {
#if AUDIO_TRACE_WRITES_HISTORY
  return samples;
#else
  (void)samples;
  return 0;
#endif
}

static uint64_t expected_snap_count(void) {
#if AUDIO_TRACE_WRITES_HISTORY
  return 2;
#else
  return 0;
#endif
}

static uint64_t expected_open_fail_snap_count(void) {
#if AUDIO_TRACE_WRITES_HISTORY
  return 3;
#else
  return 0;
#endif
}

static void expect_core_counters(uint64_t produced, uint64_t snap_count) {
  AudioTraceStats st;
  audio_trace_get_stats(&st);
  expect_u64("produced", st.produced, produced);
  expect_u64("snap_count", st.snap_count, snap_count);
}

static void force_stats_env(const char *value) {
  g_force_stats_env = 1;
  g_stats_env_value = value;
}

static void run_stats_off(void) {
  for (uint64_t i = 0; i < 1000; i++)
    emit_sample_at(i);

  expect_u64("wall calls with stats off",
             g_wall_calls, expected_off_wall_calls(1000));
  expect_u64("stats fopen calls with stats off", g_fopen_calls, 0);
  expect_core_counters(1000, 0);
}

static void run_snap_cadence(void) {
  emit_cadence_samples();

  expect_u64("wall calls with stats-off cadence",
             g_wall_calls, expected_off_wall_calls(5));
  expect_u64("stats fopen calls with stats-off cadence", g_fopen_calls, 0);
  expect_core_counters(5, expected_snap_count());
}

static void expect_stats_log(const char *path) {
  FILE *f = fopen(path, "rb");
  char line[512];
  uint64_t data_lines = 0;

  if (!f)
    fail("stats log was not created");
  if (!fgets(line, sizeof(line), f))
    fail("stats log is empty");
  if (strncmp(line, "# ms produced consumed dropped", 30) != 0)
    fail("stats log header is missing");

  while (fgets(line, sizeof(line), f)) {
    unsigned long long ms = 0;
    if (sscanf(line, "%llu", &ms) != 1)
      fail("stats log data line is malformed");
    data_lines++;
    if (data_lines == 1)
      expect_u64("first stats line ms", ms, 1000);
    else if (data_lines == 2)
      expect_u64("second stats line ms", ms, 2000);
  }
  if (ferror(f))
    fail("failed while reading stats log");
  if (fclose(f) != 0)
    fail("failed to close stats log");
  expect_u64("stats data lines", data_lines, 2);
}

static void run_stats_path(void) {
  const char *path = getenv("SNESRECOMP_AUDIO_STATS");

  if (!path || !path[0] || (path[0] == '0' && !path[1]) ||
      (path[0] == '1' && !path[1]))
    fail("stats-path mode requires SNESRECOMP_AUDIO_STATS=<path>");
  remove(path);

  emit_cadence_samples();

  expect_u64("wall calls with path stats", g_wall_calls, 7);
  expect_u64("stats fopen calls with path stats", g_fopen_calls, 1);
  expect_core_counters(5, expected_snap_count());
  expect_stats_log(path);
  remove(path);
}

static void run_stats_stderr(void) {
  const char *mode = getenv("SNESRECOMP_AUDIO_STATS");

  if (!mode || strcmp(mode, "1") != 0)
    fail("stderr mode requires SNESRECOMP_AUDIO_STATS=1");

  emit_cadence_samples();

  expect_u64("wall calls with stderr stats", g_wall_calls, 7);
  expect_u64("stats fopen calls with stderr stats", g_fopen_calls, 0);
  expect_core_counters(5, expected_snap_count());
}

static void run_stats_open_fail(void) {
  const char *path = getenv("SNESRECOMP_AUDIO_STATS");

  if (!path || !path[0] || (path[0] == '0' && !path[1]) ||
      (path[0] == '1' && !path[1]))
    fail("open-fail mode requires SNESRECOMP_AUDIO_STATS=<path>");

  emit_sample_at(30000);
  emit_sample_at(30999);
  emit_sample_at(31000);
  emit_sample_at(31999);
  emit_sample_at(32000);

  expect_u64("wall calls after stats open failure",
             g_wall_calls, expected_off_wall_calls(5));
  expect_u64("stats fopen calls after open failure", g_fopen_calls, 1);
  expect_core_counters(5, expected_open_fail_snap_count());
}

int main(int argc, char **argv) {
  if (argc != 2)
    fail("expected mode argument");
  if (strcmp(argv[1], "off") == 0) {
    run_stats_off();
  } else if (strcmp(argv[1], "off-empty") == 0) {
    force_stats_env("");
    run_stats_off();
  } else if (strcmp(argv[1], "off-zero") == 0) {
    force_stats_env("0");
    run_stats_off();
  } else if (strcmp(argv[1], "snap") == 0) {
    run_snap_cadence();
  } else if (strcmp(argv[1], "snap-empty") == 0) {
    force_stats_env("");
    run_snap_cadence();
  } else if (strcmp(argv[1], "snap-zero") == 0) {
    force_stats_env("0");
    run_snap_cadence();
  } else if (strcmp(argv[1], "path") == 0) {
    run_stats_path();
  } else if (strcmp(argv[1], "stderr") == 0) {
    run_stats_stderr();
  } else if (strcmp(argv[1], "open-fail") == 0) {
    run_stats_open_fail();
  } else {
    fail("unknown mode argument");
  }
  return 0;
}
