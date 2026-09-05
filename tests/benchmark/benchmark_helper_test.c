#include "benchmark.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

static int expect_u64(const char *name, uint64_t got, uint64_t want) {
  if (got == want) return 1;
  fprintf(stderr, "%s: got %llu want %llu\n", name,
          (unsigned long long)got, (unsigned long long)want);
  return 0;
}

static int expect_contains(const char *name, const char *haystack,
                           const char *needle) {
  if (strstr(haystack, needle)) return 1;
  fprintf(stderr, "%s: missing %s in %s\n", name, needle, haystack);
  return 0;
}

static int read_phase_json(const SnesRecompBenchmark *benchmark,
                           char *buffer, size_t buffer_size) {
  FILE *stream = tmpfile();
  if (!stream) {
    fprintf(stderr, "tmpfile failed\n");
    return 0;
  }

  SnesRecompBenchmarkPrintPhaseJson(stream, benchmark);
  fflush(stream);
  rewind(stream);

  size_t bytes_read = fread(buffer, 1, buffer_size - 1, stream);
  buffer[bytes_read] = '\0';
  fclose(stream);
  return bytes_read > 0;
}

static int expect_phase_json(const char *name,
                             const SnesRecompBenchmark *benchmark,
                             const char *phase_timing,
                             const char *phase_calls) {
  char json[2048];
  if (!read_phase_json(benchmark, json, sizeof(json))) return 0;

  int ok = 1;
  ok &= expect_contains(name, json, phase_timing);
  ok &= expect_contains(name, json, "\"phase_semantics\":\"inclusive\"");
  ok &= expect_contains(name, json, "\"phase_seconds\":{");
  ok &= expect_contains(name, json, "\"guest_frame\":");
  ok &= expect_contains(name, json, "\"ppu_draw\":");
  ok &= expect_contains(name, json, "\"host_present\":");
  ok &= expect_contains(name, json, "\"audio_render\":");
  ok &= expect_contains(name, json, "\"phase_calls\":{");
  ok &= expect_contains(name, json, phase_calls);
  return ok;
}

#if SNESRECOMP_BENCHMARK_PHASES
static uint64_t phase_call_sum(const SnesRecompBenchmark *benchmark) {
  uint64_t sum = 0;
  for (int i = 0; i < kSnesRecompBenchmarkPhase_Count; i++) {
    sum += benchmark->phase_calls[i];
  }
  return sum;
}
#endif

#if !SNESRECOMP_BENCHMARK_PHASES
static int expect_zero_phase_counters(const SnesRecompBenchmark *benchmark) {
  int ok = 1;
  for (int i = 0; i < kSnesRecompBenchmarkPhase_Count; i++) {
    char name[64];
    snprintf(name, sizeof(name), "phase-%d-ns", i);
    ok &= expect_u64(name, benchmark->phase_ns[i], 0);
    snprintf(name, sizeof(name), "phase-%d-calls", i);
    ok &= expect_u64(name, benchmark->phase_calls[i], 0);
  }
  return ok;
}
#endif

int main(void) {
  int ok = 1;

  ok &= expect_u64("zero-frequency",
                   SnesRecompBenchmarkScaleTicksToNs(123, 0), 0);
  ok &= expect_u64("one-second",
                   SnesRecompBenchmarkScaleTicksToNs(10000000ull,
                                                     10000000ull),
                   1000000000ull);
  ok &= expect_u64("fractional-second",
                   SnesRecompBenchmarkScaleTicksToNs(10000001ull,
                                                     10000000ull),
                   1000000100ull);

  /* This is just beyond the old `ticks * 1e9` uint64 overflow boundary. */
  ok &= expect_u64("old-overflow-boundary",
                   SnesRecompBenchmarkScaleTicksToNs(18446744074ull,
                                                     10000000ull),
                   1844674407400ull);

  SnesRecompBenchmark benchmark;
  SnesRecompBenchmarkBegin(&benchmark);
  volatile uint64_t sink = 0;
  for (uint64_t i = 0; i < 100000; i++) sink += i;
  SnesRecompBenchmarkEnd(&benchmark);
  if (SnesRecompBenchmarkElapsedNs(&benchmark) == 0) {
    fprintf(stderr, "elapsed time did not advance\n");
    ok = 0;
  }

#if SNESRECOMP_BENCHMARK_PHASES
  for (int phase = 0; phase < kSnesRecompBenchmarkPhase_Count; phase++) {
    uint64_t phase_start = SnesRecompBenchmarkPhaseBegin();
    for (uint64_t i = 0; i < 100000; i++) sink += i + (uint64_t)phase;
    SnesRecompBenchmarkPhaseEnd(&benchmark,
                                (SnesRecompBenchmarkPhase)phase,
                                phase_start);
    if (benchmark.phase_calls[phase] != 1) {
      fprintf(stderr, "phase %d call count did not advance\n", phase);
      ok = 0;
    }
  }

  uint64_t call_sum = phase_call_sum(&benchmark);
  SnesRecompBenchmarkPhaseEnd(
      &benchmark,
      (SnesRecompBenchmarkPhase)kSnesRecompBenchmarkPhase_Count,
      SnesRecompBenchmarkPhaseBegin());
  if (phase_call_sum(&benchmark) != call_sum) {
    fprintf(stderr, "invalid phase enum changed counters\n");
    ok = 0;
  }

  ok &= expect_phase_json(
      "phase-on-json", &benchmark, "\"phase_timing\":true",
      "\"phase_calls\":{\"guest_frame\":1,\"ppu_draw\":1,"
      "\"host_present\":1,\"audio_render\":1}");
#else
  ok &= expect_zero_phase_counters(&benchmark);
  ok &= expect_u64("phase-off-begin", SnesRecompBenchmarkPhaseBegin(), 0);
  for (int phase = 0; phase < kSnesRecompBenchmarkPhase_Count; phase++) {
    SnesRecompBenchmarkPhaseEnd(&benchmark,
                                (SnesRecompBenchmarkPhase)phase, 123);
  }
  SnesRecompBenchmarkPhaseEnd(
      &benchmark,
      (SnesRecompBenchmarkPhase)kSnesRecompBenchmarkPhase_Count, 123);
  ok &= expect_zero_phase_counters(&benchmark);
  ok &= expect_phase_json(
      "phase-off-json", &benchmark, "\"phase_timing\":false",
      "\"phase_calls\":{\"guest_frame\":0,\"ppu_draw\":0,"
      "\"host_present\":0,\"audio_render\":0}");
#endif
  (void)sink;

  return ok ? 0 : 1;
}
