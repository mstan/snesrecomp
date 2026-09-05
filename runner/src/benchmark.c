#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#include <time.h>
#endif

#include "benchmark.h"

#include <string.h>

uint64_t SnesRecompBenchmarkScaleTicksToNs(uint64_t ticks,
                                           uint64_t ticks_per_second) {
  if (!ticks_per_second) return 0;
  uint64_t whole_seconds = ticks / ticks_per_second;
  uint64_t remainder = ticks % ticks_per_second;
  return whole_seconds * 1000000000ull +
         (remainder * 1000000000ull) / ticks_per_second;
}

static uint64_t benchmark_now_ns(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return SnesRecompBenchmarkScaleTicksToNs((uint64_t)counter.QuadPart,
                                           (uint64_t)frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static double ns_to_seconds(uint64_t ns) {
  return (double)ns / 1000000000.0;
}

void SnesRecompBenchmarkBegin(SnesRecompBenchmark *benchmark) {
  memset(benchmark, 0, sizeof(*benchmark));
  benchmark->start_ns = benchmark_now_ns();
}

void SnesRecompBenchmarkEnd(SnesRecompBenchmark *benchmark) {
  benchmark->end_ns = benchmark_now_ns();
}

uint64_t SnesRecompBenchmarkElapsedNs(
    const SnesRecompBenchmark *benchmark) {
  return benchmark->end_ns >= benchmark->start_ns
             ? benchmark->end_ns - benchmark->start_ns
             : 0;
}

double SnesRecompBenchmarkElapsedSeconds(
    const SnesRecompBenchmark *benchmark) {
  return ns_to_seconds(SnesRecompBenchmarkElapsedNs(benchmark));
}

#if SNESRECOMP_BENCHMARK_PHASES
uint64_t SnesRecompBenchmarkPhaseBegin(void) {
  return benchmark_now_ns();
}

void SnesRecompBenchmarkPhaseEnd(SnesRecompBenchmark *benchmark,
                                 SnesRecompBenchmarkPhase phase,
                                 uint64_t start_ns) {
  if ((unsigned)phase >= (unsigned)kSnesRecompBenchmarkPhase_Count) return;
  uint64_t end_ns = benchmark_now_ns();
  benchmark->phase_ns[phase] += end_ns >= start_ns ? end_ns - start_ns : 0;
  benchmark->phase_calls[phase]++;
}
#endif

void SnesRecompBenchmarkPrintPhaseJson(
    FILE *stream, const SnesRecompBenchmark *benchmark) {
#if SNESRECOMP_BENCHMARK_PHASES
  const int enabled = 1;
  const uint64_t guest_frame_ns =
      benchmark->phase_ns[kSnesRecompBenchmarkPhase_GuestFrame];
  const uint64_t ppu_draw_ns =
      benchmark->phase_ns[kSnesRecompBenchmarkPhase_PpuDraw];
  const uint64_t host_present_ns =
      benchmark->phase_ns[kSnesRecompBenchmarkPhase_HostPresent];
  const uint64_t audio_render_ns =
      benchmark->phase_ns[kSnesRecompBenchmarkPhase_AudioRender];
  const uint64_t guest_frame_calls =
      benchmark->phase_calls[kSnesRecompBenchmarkPhase_GuestFrame];
  const uint64_t ppu_draw_calls =
      benchmark->phase_calls[kSnesRecompBenchmarkPhase_PpuDraw];
  const uint64_t host_present_calls =
      benchmark->phase_calls[kSnesRecompBenchmarkPhase_HostPresent];
  const uint64_t audio_render_calls =
      benchmark->phase_calls[kSnesRecompBenchmarkPhase_AudioRender];
#else
  (void)benchmark;
  const int enabled = 0;
  const uint64_t guest_frame_ns = 0;
  const uint64_t ppu_draw_ns = 0;
  const uint64_t host_present_ns = 0;
  const uint64_t audio_render_ns = 0;
  const uint64_t guest_frame_calls = 0;
  const uint64_t ppu_draw_calls = 0;
  const uint64_t host_present_calls = 0;
  const uint64_t audio_render_calls = 0;
#endif
  fprintf(stream,
          "\"phase_timing\":%s,"
          "\"phase_semantics\":\"inclusive\","
          "\"phase_seconds\":{"
          "\"guest_frame\":%.9f,"
          "\"ppu_draw\":%.9f,"
          "\"host_present\":%.9f,"
          "\"audio_render\":%.9f"
          "},"
          "\"phase_calls\":{"
          "\"guest_frame\":%llu,"
          "\"ppu_draw\":%llu,"
          "\"host_present\":%llu,"
          "\"audio_render\":%llu"
          "}",
          enabled ? "true" : "false", ns_to_seconds(guest_frame_ns),
          ns_to_seconds(ppu_draw_ns), ns_to_seconds(host_present_ns),
          ns_to_seconds(audio_render_ns),
          (unsigned long long)guest_frame_calls,
          (unsigned long long)ppu_draw_calls,
          (unsigned long long)host_present_calls,
          (unsigned long long)audio_render_calls);
}
