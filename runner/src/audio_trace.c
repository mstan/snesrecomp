#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "audio_trace.h"

#ifdef SNESRECOMP_AUDIO_TRACE_TEST_WALL_MS
extern uint64_t audio_trace_test_wall_ms(void);
static uint64_t wall_ms(void) { return audio_trace_test_wall_ms(); }
#define AUDIO_TRACE_WALL_MS_DEFINED 1
#endif

#ifdef SNES_COSIM
/* Co-sim determinism (SNES_COSIM.md, Gate 1): every audio-pacing consumer that
 * reads the wall clock (APU catch-up baseline, RtlApuWrite port-write scheduler)
 * would otherwise diverge run-to-run. Replace the host clock at its source with
 * a VIRTUAL clock derived purely from guest execution (the 21.47727 MHz master
 * clock: ~46.5665 ns / 21477.27 cyc-per-ms per master cycle), so two runs pace
 * the SPC and schedule port writes identically. Dev/diagnostics only.
 * (mc*4657/100 ≈ mc*46.57 ns; no overflow for years.) */
#ifdef SNES_COSIM_REF
/* B side: the interp816 driver owns the master-cycle accumulator (no g_cpu). */
extern uint64_t g_ref_master_cycles;
#ifndef AUDIO_TRACE_WALL_MS_DEFINED
static uint64_t wall_ms(void) { return g_ref_master_cycles / 21477ull; }
#endif
static uint64_t wall_ns(void) { return g_ref_master_cycles * 4657ull / 100ull; }
#else
#include "cpu_state.h"
extern CpuState g_cpu;
#ifndef AUDIO_TRACE_WALL_MS_DEFINED
static uint64_t wall_ms(void) { return g_cpu.master_cycles / 21477ull; }
#endif
static uint64_t wall_ns(void) { return g_cpu.master_cycles * 4657ull / 100ull; }
#endif
#elif defined(_WIN32)
#include <windows.h>
#ifndef AUDIO_TRACE_WALL_MS_DEFINED
static uint64_t wall_ms(void) { return (uint64_t)GetTickCount64(); }
#endif
/* High-resolution monotonic nanoseconds — GetTickCount64's ~15 ms
 * granularity is far too coarse for intra-frame port-write spacing. */
static uint64_t wall_ns(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER now;
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}
#else
#include <time.h>
#ifndef AUDIO_TRACE_WALL_MS_DEFINED
static uint64_t wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif
static uint64_t wall_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
#endif

/* Provided by the game's main.c — serialises against both APU producers. */
void RtlApuLock(void);
void RtlApuUnlock(void);

#ifdef SNESRECOMP_AUDIO_TRACE_TEST_FOPEN
extern FILE *audio_trace_test_fopen(const char *path, const char *mode);
#define AUDIO_TRACE_FOPEN(path, mode) audio_trace_test_fopen((path), (mode))
#else
#define AUDIO_TRACE_FOPEN(path, mode) fopen((path), (mode))
#endif

#ifdef SNESRECOMP_AUDIO_TRACE_TEST_GETENV
extern char *audio_trace_test_getenv(const char *name);
#define AUDIO_TRACE_GETENV(name) audio_trace_test_getenv((name))
#else
#define AUDIO_TRACE_GETENV(name) getenv((name))
#endif

#if AUDIO_TRACE_HAS_STORAGE
static int16_t s_pcm[AUDIO_TRACE_PCM_RING * 2];
static AudioTraceEvent s_events[AUDIO_TRACE_EVENT_RING];
static AudioTraceSnap s_snaps[AUDIO_TRACE_SNAP_RING];
#endif
static AudioTraceStats s_stats;
static int s_producer = AUDIO_TRACE_PRODUCER_UNKNOWN;
/* Open drop run: index into s_events of the DROP event being extended,
 * or UINT64_MAX when the last recorded sample was not dropped. */
static uint64_t s_open_drop_event = UINT64_MAX;
static uint64_t s_last_snap_ms = 0;

static AudioTraceEvent *push_event(uint8_t type) {
#if AUDIO_TRACE_WRITES_HISTORY
  AudioTraceEvent *e = &s_events[s_stats.event_count & (AUDIO_TRACE_EVENT_RING - 1)];
  s_stats.event_count++;
  e->sample_idx = s_stats.produced;
  e->aux = 0;
  e->type = type;
  e->addr = 0;
  e->val = 0;
  e->producer = (uint8_t)s_producer;
  return e;
#else
  static AudioTraceEvent e;
  s_stats.event_count++;
  e.sample_idx = s_stats.produced;
  e.aux = 0;
  e.type = type;
  e.addr = 0;
  e.val = 0;
  e.producer = (uint8_t)s_producer;
  return &e;
#endif
}

/* Release-readable readout of the always-on counters.
 *
 * The debug server (audio_stats / audio_wav) compiles to nothing without
 * SNESRECOMP_TRACE, so on a Production build — the one the user actually
 * listens to — there was no way to read these rings at all. A trace build
 * is not an acceptable substitute here: it perturbs the very timing the
 * audio path is being measured for.
 *
 * The counters have been accumulating since process start regardless of the
 * retained-history mode, and setting SNESRECOMP_AUDIO_STATS only decides
 * whether a line gets written out once per second. Line-buffered + explicit
 * fflush so a killed process still leaves every completed second on disk.
 *
 * SNESRECOMP_AUDIO_STATS=<path>  append one line per second to <path>
 * SNESRECOMP_AUDIO_STATS=1       ... to stderr
 */
static FILE *s_stats_out;        /* NULL until resolved; s_stats_mode says how */
static int   s_stats_mode = -1;  /* -1 unresolved, 0 off, 1 on */
static int   s_stats_header_written;

static int stats_mode(void) {
  if (s_stats_mode < 0) {
    const char *e = AUDIO_TRACE_GETENV("SNESRECOMP_AUDIO_STATS");
    if (!e || !e[0] || (e[0] == '0' && !e[1])) {
      s_stats_mode = 0;
    } else if (e[0] == '1' && !e[1]) {
      s_stats_out = stderr;
      s_stats_mode = 1;
    } else {
      s_stats_out = AUDIO_TRACE_FOPEN(e, "a");
      s_stats_mode = s_stats_out ? 1 : 0;
    }
  }
  return s_stats_mode;
}

static void stats_line(uint32_t ring_fill) {
  if (stats_mode() == 0) return;
  if (!s_stats_header_written) {
    fprintf(s_stats_out,
            "# ms produced consumed dropped dropped_audible drop_runs "
            "underflows consume_calls occupancy hiwater prod_cpu prod_audio\n");
    s_stats_header_written = 1;
  }
  fprintf(s_stats_out,
          "%llu %llu %llu %llu %llu %llu %llu %llu %u %u %llu %llu\n",
          (unsigned long long)wall_ms(),
          (unsigned long long)s_stats.produced,
          (unsigned long long)s_stats.consumed,
          (unsigned long long)s_stats.dropped,
          (unsigned long long)s_stats.dropped_audible,
          (unsigned long long)s_stats.drop_runs,
          (unsigned long long)s_stats.output_underflows,
          (unsigned long long)s_stats.consume_calls,
          ring_fill, s_stats.occupancy_highwater,
          (unsigned long long)s_stats.produced_cpu,
          (unsigned long long)s_stats.produced_audio);
  fflush(s_stats_out);
}

static void maybe_snap(uint32_t ring_fill) {
#if !AUDIO_TRACE_WRITES_HISTORY
  if (stats_mode() == 0) return;
#endif
  uint64_t now = wall_ms();
  if (now - s_last_snap_ms < 1000) return;
  s_last_snap_ms = now;
#if AUDIO_TRACE_WRITES_HISTORY
  AudioTraceSnap *s = &s_snaps[s_stats.snap_count & (AUDIO_TRACE_SNAP_RING - 1)];
  s_stats.snap_count++;
  s->wall_ms = now;
  s->produced = s_stats.produced;
  s->dropped = s_stats.dropped;
  s->consumed = s_stats.consumed;
  s->occupancy = ring_fill;
#endif
  stats_line(ring_fill);
}

void audio_trace_set_producer(int producer) {
  s_producer = producer;
}

void audio_trace_on_fast_forward_discard(uint32_t samples,
                                         uint32_t occupancy_after) {
  s_stats.fast_forward_discarded += samples;
  s_stats.occupancy_current = occupancy_after;
}

void audio_trace_on_output_underflow(uint32_t occupancy) {
  s_stats.output_underflows++;
  s_stats.occupancy_current = occupancy;
}

void audio_trace_on_sample(int16_t l, int16_t r, int dropped, uint32_t ring_fill) {
#if AUDIO_TRACE_WRITES_HISTORY
  uint32_t w = (uint32_t)(s_stats.produced & (AUDIO_TRACE_PCM_RING - 1));
  s_pcm[w * 2] = l;
  s_pcm[w * 2 + 1] = r;
#endif
  if (dropped) {
    if (s_open_drop_event != UINT64_MAX &&
        s_stats.event_count - s_open_drop_event <=
            AUDIO_TRACE_DROP_RUN_EVENT_RING) {
#if AUDIO_TRACE_WRITES_HISTORY
      s_events[s_open_drop_event & (AUDIO_TRACE_EVENT_RING - 1)].aux++;
#endif
    } else {
      s_open_drop_event = s_stats.event_count;
      push_event(AUDIO_TRACE_EV_DROP)->aux = 1;
      s_stats.drop_runs++;
    }
    s_stats.dropped++;
    /* Silent-vs-audible attribution: ring-overflow drops during SPC upload
     * phases discard digital silence (measured -84 dBFS across a stage
     * load) — harmless. A drop carrying real signal is the regression that
     * matters. Threshold 256/32768 ~= -42 dBFS, well above idle noise. */
    if (l > 256 || l < -256 || r > 256 || r < -256)
      s_stats.dropped_audible++;
  } else {
    s_open_drop_event = UINT64_MAX;
  }
  s_stats.produced++;
  if (s_producer == AUDIO_TRACE_PRODUCER_CPU) s_stats.produced_cpu++;
  else if (s_producer == AUDIO_TRACE_PRODUCER_AUDIO) s_stats.produced_audio++;
  if (ring_fill > s_stats.occupancy_highwater) s_stats.occupancy_highwater = ring_fill;
  s_stats.occupancy_current = ring_fill;
  maybe_snap(ring_fill);
}

void audio_trace_on_reg_write(uint8_t addr, uint8_t val) {
  AudioTraceEvent *e = push_event(AUDIO_TRACE_EV_REG);
  e->addr = addr;
  e->val = val;
  s_stats.reg_writes++;
  if (addr == 0x4c && val != 0) s_stats.kon_writes++;
  s_open_drop_event = UINT64_MAX;
}

void audio_trace_on_shadow_div(double dl, double dr) {
  double a = dl < 0 ? -dl : dl;
  double b = dr < 0 ? -dr : dr;
  if (a > s_stats.shadow_div_max) s_stats.shadow_div_max = a;
  if (b > s_stats.shadow_div_max) s_stats.shadow_div_max = b;
  s_stats.shadow_div_sumsq += (dl * dl + dr * dr) * 0.5;
  s_stats.shadow_div_count++;
}

void audio_trace_on_faithful_div(double d) {
  double a = d < 0 ? -d : d;
  if (a > s_stats.faithful_div_max) s_stats.faithful_div_max = a;
  s_stats.faithful_div_sumsq += d * d;
  s_stats.faithful_div_count++;
}

void audio_trace_on_brr_compare(uint16_t block, uint8_t header, uint8_t sample,
                                int canon, int reference, int old, int older) {
  double d = (double)(canon - reference) / 32768.0;
  double a = d < 0 ? -d : d;
  if (a > s_stats.brr_div_max) s_stats.brr_div_max = a;
  s_stats.brr_div_sumsq += d * d;
  s_stats.brr_div_count++;
  if (canon != reference && !s_stats.brr_first_valid) {
    s_stats.brr_first_valid = 1;
    s_stats.brr_first_block = block;
    s_stats.brr_first_header = header;
    s_stats.brr_first_sample = sample;
    s_stats.brr_first_canon = canon;
    s_stats.brr_first_reference = reference;
    s_stats.brr_first_old = old;
    s_stats.brr_first_older = older;
  }
}

void audio_trace_on_echo_div(double d) {
  double a = d < 0 ? -d : d;
  if (a > s_stats.echo_div_max) s_stats.echo_div_max = a;
  s_stats.echo_div_sumsq += d * d;
  s_stats.echo_div_count++;
}

void audio_trace_on_pace(int consumer_active, uint32_t baseline_cycles) {
  s_stats.pace_consumer_active = (uint32_t)(consumer_active != 0);
  s_stats.pace_baseline_cycles += baseline_cycles;
  s_stats.pace_accumulate_calls++;
}

void audio_trace_on_guest_sync(int frame_boundary, uint64_t cycles) {
  if (frame_boundary)
    s_stats.guest_frame_sync_cycles += cycles;
  else
    s_stats.guest_read_sync_cycles += cycles;
}

/* ---- CPU<->SPC port traffic ----
 * All hooks run under RtlApuLock (RtlApuWrite / snes_readBBus take it;
 * apu_cycle is only reached with it held), so plain fields suffice.
 * Gating state per port:
 *   - s_spc_rd_last/s_cpu_rd_last: last value the reader saw; unchanged
 *     re-reads (steady-state polling) are elided from the ring.
 *   - s_spc_rd_fresh/s_cpu_rd_fresh: counterpart wrote since the last
 *     recorded read, so the next read is recorded even if the value is
 *     unchanged (same sound ID queued twice must show two observations).
 *   - s_cpu_wr_pending: a CPU port write not yet observed by any SPC
 *     read; a second CPU write while pending increments the per-port
 *     overwrite counter — the "engine never saw it" drop signature. */
extern int snes_frame_counter; /* common_rtl.c — game frame number */
static uint8_t s_spc_rd_last[4], s_cpu_rd_last[4];
static uint8_t s_spc_rd_fresh[4], s_cpu_rd_fresh[4];
static uint8_t s_cpu_wr_pending[4];

static AudioTraceEvent *push_port_event(uint8_t type, uint8_t port, uint8_t val) {
  AudioTraceEvent *e = push_event(type);
  e->addr = port;
  e->val = val;
  e->aux = (uint32_t)snes_frame_counter;
  return e;
}

void audio_trace_on_cpu_port_write(uint8_t port, uint8_t val) {
  /* Request only — the write is queued and lands in inPorts at its
   * scheduled APU-sample target. Loss accounting and SPC-read gating
   * key off the APPLY hook below, where the engine can actually see
   * the value. */
  port &= 3;
  s_stats.cpu_port_writes++;
  push_port_event(AUDIO_TRACE_EV_CPU_PORT_WRITE, port, val);
}

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t val) {
  port &= 3;
  if (s_cpu_wr_pending[port])
    s_stats.cpu_port_overwrites[port]++;
  /* Only a NONZERO value is a command that can be lost; the per-frame
   * zero-writes (SMW NMI clears the mirrors) just retire the port. */
  s_cpu_wr_pending[port] = (uint8_t)(val != 0);
  s_spc_rd_fresh[port] = 1;
  push_port_event(AUDIO_TRACE_EV_CPU_PORT_APPLY, port, val);
}

void audio_trace_on_spc_port_read(uint8_t port, uint8_t val) {
  port &= 3;
  s_stats.spc_port_reads_seen++;
  s_cpu_wr_pending[port] = 0;
  if (!s_spc_rd_fresh[port] && val == s_spc_rd_last[port]) return;
  s_spc_rd_fresh[port] = 0;
  s_spc_rd_last[port] = val;
  s_stats.spc_port_reads_logged++;
  push_port_event(AUDIO_TRACE_EV_SPC_PORT_READ, port, val);
}

void audio_trace_on_spc_port_write(uint8_t port, uint8_t val) {
  port &= 3;
  s_stats.spc_port_writes++;
  s_cpu_rd_fresh[port] = 1;
  /* Engine outPort writes are frequent (per-tick echoes); record only
   * value changes. The raw total is still counted above. */
  static uint8_t last[4];
  if (val == last[port]) return;
  last[port] = val;
  push_port_event(AUDIO_TRACE_EV_SPC_PORT_WRITE, port, val);
}

void audio_trace_on_cpu_port_read(uint8_t port, uint8_t val) {
  port &= 3;
  if (!s_cpu_rd_fresh[port] && val == s_cpu_rd_last[port]) return;
  s_cpu_rd_fresh[port] = 0;
  s_cpu_rd_last[port] = val;
  s_stats.cpu_port_reads_logged++;
  push_port_event(AUDIO_TRACE_EV_CPU_PORT_READ, port, val);
}

uint64_t audio_trace_wall_ms(void) {
  return wall_ms();
}

static uint32_t s_max_consume_chunk;

void audio_trace_on_consume(uint64_t read_idx, uint32_t count, uint32_t avail_after) {
  AudioTraceEvent *e = push_event(AUDIO_TRACE_EV_CONSUME);
  e->aux = avail_after;
  (void)read_idx;
  s_stats.consumed += count;
  s_stats.consume_calls++;
  s_stats.occupancy_current = avail_after;
  if (count > s_max_consume_chunk) s_max_consume_chunk = count;
  s_open_drop_event = UINT64_MAX;
}

uint64_t audio_trace_wall_ns(void) {
  return wall_ns();
}

uint32_t audio_trace_consume_quantum(void) {
  /* Largest native-sample chunk an audio callback has consumed — the
   * APU's burst granularity. audio_samples in config.ini (and host-rate
   * resampling) make this per-game and per-user; 534 (one DSP block,
   * 32040/60) is the floor before the first callback. */
  return s_max_consume_chunk > 534u ? s_max_consume_chunk : 534u;
}

void audio_trace_sample_clocks(uint64_t *produced, uint64_t *consumed) {
  if (produced) *produced = s_stats.produced;
  if (consumed) *consumed = s_stats.consumed;
}

void audio_trace_get_stats(AudioTraceStats *out) {
  RtlApuLock();
#if AUDIO_TRACE_HAS_STORAGE && !AUDIO_TRACE_WRITES_HISTORY
  static volatile uintptr_t s_storage_anchor;
  s_storage_anchor ^= (uintptr_t)&s_pcm[0];
  s_storage_anchor ^= (uintptr_t)&s_events[0];
  s_storage_anchor ^= (uintptr_t)&s_snaps[0];
#endif
  *out = s_stats;
  RtlApuUnlock();
}

uint32_t audio_trace_copy_events(uint64_t first_idx, uint32_t max,
                                 AudioTraceEvent *out, uint64_t *oldest) {
  RtlApuLock();
#if AUDIO_TRACE_WRITES_HISTORY
  uint64_t total = s_stats.event_count;
  uint64_t old = total > AUDIO_TRACE_EVENT_RING ? total - AUDIO_TRACE_EVENT_RING : 0;
  if (oldest) *oldest = old;
  if (first_idx < old) first_idx = old;
  uint32_t n = 0;
  while (first_idx + n < total && n < max) {
    out[n] = s_events[(first_idx + n) & (AUDIO_TRACE_EVENT_RING - 1)];
    n++;
  }
  RtlApuUnlock();
  return n;
#else
  if (oldest) *oldest = s_stats.event_count;
  (void)first_idx;
  (void)max;
  (void)out;
  RtlApuUnlock();
  return 0;
#endif
}

uint32_t audio_trace_copy_snaps(uint64_t first_idx, uint32_t max,
                                AudioTraceSnap *out, uint64_t *oldest) {
  RtlApuLock();
#if AUDIO_TRACE_WRITES_HISTORY
  uint64_t total = s_stats.snap_count;
  uint64_t old = total > AUDIO_TRACE_SNAP_RING ? total - AUDIO_TRACE_SNAP_RING : 0;
  if (oldest) *oldest = old;
  if (first_idx < old) first_idx = old;
  uint32_t n = 0;
  while (first_idx + n < total && n < max) {
    out[n] = s_snaps[(first_idx + n) & (AUDIO_TRACE_SNAP_RING - 1)];
    n++;
  }
  RtlApuUnlock();
  return n;
#else
  if (oldest) *oldest = s_stats.snap_count;
  (void)first_idx;
  (void)max;
  (void)out;
  RtlApuUnlock();
  return 0;
#endif
}

int audio_trace_dump_wav(const char *path, int64_t start_idx, uint64_t count,
                         uint64_t *out_start, uint64_t *out_count) {
#if !AUDIO_TRACE_WRITES_HISTORY
  (void)path;
  (void)start_idx;
  (void)count;
  if (out_start) *out_start = 0;
  if (out_count) *out_count = 0;
  return -1;
#else
  if (!path || !path[0]) {
    if (out_start) *out_start = 0;
    if (out_count) *out_count = 0;
    return -1;
  }

  uint64_t copy_cap = count ? count : AUDIO_TRACE_PCM_RING;
  if (copy_cap > AUDIO_TRACE_PCM_RING)
    copy_cap = AUDIO_TRACE_PCM_RING;
  if (copy_cap > (uint64_t)(SIZE_MAX / (2 * sizeof(int16_t)))) {
    fprintf(stderr, "audio_trace_dump_wav: requested slice is too large\n");
    if (out_start) *out_start = 0;
    if (out_count) *out_count = 0;
    return -1;
  }

  int16_t *snapshot = NULL;
  if (copy_cap > 0) {
    snapshot = (int16_t *)malloc((size_t)copy_cap * 2 * sizeof(int16_t));
    if (!snapshot) {
      fprintf(stderr, "audio_trace_dump_wav: out of memory for %llu samples\n",
              (unsigned long long)copy_cap);
      if (out_start) *out_start = 0;
      if (out_count) *out_count = 0;
      return -1;
    }
  }

  /* Copy the requested PCM slice while the APU lock freezes producers. Do not
   * hold the lock over allocation or file I/O; SMALL history is short enough
   * that live producers can otherwise lap the ring while fwrite is running. */
  RtlApuLock();
  uint64_t total = s_stats.produced;
  uint64_t oldest = total > AUDIO_TRACE_PCM_RING ? total - AUDIO_TRACE_PCM_RING : 0;
  uint64_t start = (start_idx < 0) ? oldest : (uint64_t)start_idx;
  if (start < oldest) start = oldest;
  if (start > total) start = total;
  uint64_t avail = total - start;
  if (count == 0 || count > avail) count = avail;
  if (count > copy_cap)
    count = copy_cap;
  for (uint64_t i = 0; i < count; ) {
    uint32_t r = (uint32_t)((start + i) & (AUDIO_TRACE_PCM_RING - 1));
    uint64_t run = AUDIO_TRACE_PCM_RING - r;
    if (run > count - i) run = count - i;
    memcpy(&snapshot[i * 2], &s_pcm[(uint64_t)r * 2],
           (size_t)run * 2 * sizeof(int16_t));
    i += run;
  }
  RtlApuUnlock();

  FILE *f = AUDIO_TRACE_FOPEN(path, "wb");
  if (!f) {
    fprintf(stderr, "audio_trace_dump_wav: cannot open %s\n", path);
    free(snapshot);
    if (out_start) *out_start = 0;
    if (out_count) *out_count = 0;
    return -1;
  }
  uint32_t data_bytes = (uint32_t)(count * 4);
  /* The PCM ring stores DSP output at the S-DSP NATIVE rate, which is 32040 Hz
   * (1364*262*60 master / 32 per sample; the same rate apuCyclesPerMaster and
   * the config default are derived from, and byuu's measured real-SNES DSP
   * rate). A previous 32000 here mislabeled the dump: every drift-tolerant A/B
   * (tools/audio_ab_diff.py) then resampled the recomp 32000->32040 against the
   * 32040 oracle, stretching it ~1250 ppm and misaligning onsets -- inflating
   * the apparent "off-cue" (measured 2026-06-28: fixing this alone moved SMW
   * drift -4013 -> -2272 ppm and onset match 53% -> 71%). Label the true rate. */
  uint32_t sample_rate = 32040; /* native S-DSP rate (see above) */
  uint32_t byte_rate = sample_rate * 4;
  uint32_t riff_size = 36 + data_bytes;
  uint16_t fmt16;
  uint32_t fmt32;
  int ok = 1;
#define WAV_WRITE(ptr, size, nmemb) \
  do { if (fwrite((ptr), (size), (nmemb), f) != (nmemb)) ok = 0; } while (0)
  WAV_WRITE("RIFF", 1, 4);
  WAV_WRITE(&riff_size, 4, 1);
  WAV_WRITE("WAVEfmt ", 1, 8);
  fmt32 = 16;          WAV_WRITE(&fmt32, 4, 1); /* fmt chunk size  */
  fmt16 = 1;           WAV_WRITE(&fmt16, 2, 1); /* PCM             */
  fmt16 = 2;           WAV_WRITE(&fmt16, 2, 1); /* stereo          */
  WAV_WRITE(&sample_rate, 4, 1);
  WAV_WRITE(&byte_rate, 4, 1);
  fmt16 = 4;           WAV_WRITE(&fmt16, 2, 1); /* block align     */
  fmt16 = 16;          WAV_WRITE(&fmt16, 2, 1); /* bits per sample */
  WAV_WRITE("data", 1, 4);
  WAV_WRITE(&data_bytes, 4, 1);
  if (count > 0)
    WAV_WRITE(snapshot, 4, (size_t)count);
#undef WAV_WRITE
  if (fclose(f) != 0)
    ok = 0;
  free(snapshot);
  if (!ok) {
    fprintf(stderr, "audio_trace_dump_wav: write failed for %s\n", path);
    remove(path);
    if (out_start) *out_start = 0;
    if (out_count) *out_count = 0;
    return -1;
  }
  if (out_start) *out_start = start;
  if (out_count) *out_count = count;
  return 0;
#endif
}
