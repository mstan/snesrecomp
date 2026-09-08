#ifndef SNESRECOMP_BENCHMARK_H
#define SNESRECOMP_BENCHMARK_H

#include <stdint.h>
#include <stdio.h>

#ifndef SNESRECOMP_BENCHMARK_PHASES
#define SNESRECOMP_BENCHMARK_PHASES 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SnesRecompBenchmarkPhase {
  kSnesRecompBenchmarkPhase_GuestFrame = 0,
  kSnesRecompBenchmarkPhase_PpuDraw = 1,
  kSnesRecompBenchmarkPhase_HostPresent = 2,
  kSnesRecompBenchmarkPhase_AudioRender = 3,
  kSnesRecompBenchmarkPhase_Count = 4,
} SnesRecompBenchmarkPhase;

typedef struct SnesRecompBenchmark {
  uint64_t start_ns;
  uint64_t end_ns;
  uint64_t phase_ns[kSnesRecompBenchmarkPhase_Count];
  uint64_t phase_calls[kSnesRecompBenchmarkPhase_Count];
} SnesRecompBenchmark;

uint64_t SnesRecompBenchmarkScaleTicksToNs(uint64_t ticks,
                                           uint64_t ticks_per_second);
void SnesRecompBenchmarkBegin(SnesRecompBenchmark *benchmark);
void SnesRecompBenchmarkEnd(SnesRecompBenchmark *benchmark);
uint64_t SnesRecompBenchmarkElapsedNs(const SnesRecompBenchmark *benchmark);
double SnesRecompBenchmarkElapsedSeconds(
    const SnesRecompBenchmark *benchmark);
void SnesRecompBenchmarkPrintPhaseJson(
    FILE *stream, const SnesRecompBenchmark *benchmark);

#if SNESRECOMP_BENCHMARK_PHASES
uint64_t SnesRecompBenchmarkPhaseBegin(void);
void SnesRecompBenchmarkPhaseEnd(SnesRecompBenchmark *benchmark,
                                 SnesRecompBenchmarkPhase phase,
                                 uint64_t start_ns);
#else
static inline uint64_t SnesRecompBenchmarkPhaseBegin(void) { return 0; }
static inline void SnesRecompBenchmarkPhaseEnd(
    SnesRecompBenchmark *benchmark, SnesRecompBenchmarkPhase phase,
    uint64_t start_ns) {
  (void)benchmark;
  (void)phase;
  (void)start_ns;
}
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif
