#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "audio_trace.h"

#ifdef _WIN32
#include <windows.h>
static uint64_t wall_ms(void) { return (uint64_t)GetTickCount64(); }
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
static uint64_t wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
static uint64_t wall_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
#endif

/* Provided by the game's main.c — serialises against both APU producers. */
void RtlApuLock(void);
void RtlApuUnlock(void);

static int16_t s_pcm[AUDIO_TRACE_PCM_RING * 2];
static AudioTraceEvent s_events[AUDIO_TRACE_EVENT_RING];
static AudioTraceSnap s_snaps[AUDIO_TRACE_SNAP_RING];
static AudioTraceStats s_stats;
static int s_producer = AUDIO_TRACE_PRODUCER_UNKNOWN;
/* Open drop run: index into s_events of the DROP event being extended,
 * or UINT64_MAX when the last recorded sample was not dropped. */
static uint64_t s_open_drop_event = UINT64_MAX;
static uint64_t s_last_snap_ms = 0;

static AudioTraceEvent *push_event(uint8_t type) {
  AudioTraceEvent *e = &s_events[s_stats.event_count & (AUDIO_TRACE_EVENT_RING - 1)];
  s_stats.event_count++;
  e->sample_idx = s_stats.produced;
  e->aux = 0;
  e->type = type;
  e->addr = 0;
  e->val = 0;
  e->producer = (uint8_t)s_producer;
  return e;
}

static void maybe_snap(uint32_t ring_fill) {
  uint64_t now = wall_ms();
  if (now - s_last_snap_ms < 1000) return;
  s_last_snap_ms = now;
  AudioTraceSnap *s = &s_snaps[s_stats.snap_count & (AUDIO_TRACE_SNAP_RING - 1)];
  s_stats.snap_count++;
  s->wall_ms = now;
  s->produced = s_stats.produced;
  s->dropped = s_stats.dropped;
  s->consumed = s_stats.consumed;
  s->occupancy = ring_fill;
}

void audio_trace_set_producer(int producer) {
  s_producer = producer;
}

void audio_trace_on_sample(int16_t l, int16_t r, int dropped, uint32_t ring_fill) {
  uint32_t w = (uint32_t)(s_stats.produced & (AUDIO_TRACE_PCM_RING - 1));
  s_pcm[w * 2] = l;
  s_pcm[w * 2 + 1] = r;
  if (dropped) {
    if (s_open_drop_event != UINT64_MAX &&
        s_stats.event_count - s_open_drop_event <= AUDIO_TRACE_EVENT_RING) {
      s_events[s_open_drop_event & (AUDIO_TRACE_EVENT_RING - 1)].aux++;
    } else {
      s_open_drop_event = s_stats.event_count;
      push_event(AUDIO_TRACE_EV_DROP)->aux = 1;
      s_stats.drop_runs++;
    }
    s_stats.dropped++;
  } else {
    s_open_drop_event = UINT64_MAX;
  }
  s_stats.produced++;
  if (s_producer == AUDIO_TRACE_PRODUCER_CPU) s_stats.produced_cpu++;
  else if (s_producer == AUDIO_TRACE_PRODUCER_AUDIO) s_stats.produced_audio++;
  if (ring_fill > s_stats.occupancy_highwater) s_stats.occupancy_highwater = ring_fill;
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

void audio_trace_on_brr_div(double d) {
  double a = d < 0 ? -d : d;
  if (a > s_stats.brr_div_max) s_stats.brr_div_max = a;
  s_stats.brr_div_sumsq += d * d;
  s_stats.brr_div_count++;
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
  *out = s_stats;
  RtlApuUnlock();
}

uint32_t audio_trace_copy_events(uint64_t first_idx, uint32_t max,
                                 AudioTraceEvent *out, uint64_t *oldest) {
  RtlApuLock();
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
}

uint32_t audio_trace_copy_snaps(uint64_t first_idx, uint32_t max,
                                AudioTraceSnap *out, uint64_t *oldest) {
  RtlApuLock();
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
}

int audio_trace_dump_wav(const char *path, int64_t start_idx, uint64_t count,
                         uint64_t *out_start, uint64_t *out_count) {
  /* Snapshot the write head under the lock; the slice [oldest, total) is
   * then stable without the lock — ring slots are append-only and only
   * lapped after a full 131 s revolution, far longer than any dump. */
  RtlApuLock();
  uint64_t total = s_stats.produced;
  RtlApuUnlock();
  uint64_t oldest = total > AUDIO_TRACE_PCM_RING ? total - AUDIO_TRACE_PCM_RING : 0;
  uint64_t start = (start_idx < 0) ? oldest : (uint64_t)start_idx;
  if (start < oldest) start = oldest;
  if (start > total) start = total;
  uint64_t avail = total - start;
  if (count == 0 || count > avail) count = avail;

  FILE *f = fopen(path, "wb");
  if (!f) return -1;
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
  fwrite("RIFF", 1, 4, f);
  fwrite(&riff_size, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  fmt32 = 16;          fwrite(&fmt32, 4, 1, f); /* fmt chunk size  */
  fmt16 = 1;           fwrite(&fmt16, 2, 1, f); /* PCM             */
  fmt16 = 2;           fwrite(&fmt16, 2, 1, f); /* stereo          */
  fwrite(&sample_rate, 4, 1, f);
  fwrite(&byte_rate, 4, 1, f);
  fmt16 = 4;           fwrite(&fmt16, 2, 1, f); /* block align     */
  fmt16 = 16;          fwrite(&fmt16, 2, 1, f); /* bits per sample */
  fwrite("data", 1, 4, f);
  fwrite(&data_bytes, 4, 1, f);
  for (uint64_t i = 0; i < count; ) {
    uint32_t r = (uint32_t)((start + i) & (AUDIO_TRACE_PCM_RING - 1));
    /* contiguous run up to the ring wrap point */
    uint64_t run = AUDIO_TRACE_PCM_RING - r;
    if (run > count - i) run = count - i;
    fwrite(&s_pcm[(uint64_t)r * 2], 4, (size_t)run, f);
    i += run;
  }
  fclose(f);
  if (out_start) *out_start = start;
  if (out_count) *out_count = count;
  return 0;
}
