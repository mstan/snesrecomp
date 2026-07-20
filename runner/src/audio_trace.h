#ifndef AUDIO_TRACE_H
#define AUDIO_TRACE_H

/* Always-on audio observability rings.
 *
 * Captures, continuously from process start (Release/Production too, per
 * the ring-buffer observability discipline):
 *
 *   1. PCM ring   — every native DSP output sample dsp_cycle produces,
 *                   recorded BEFORE the output-ring overflow check, so
 *                   samples dropped at the dsp->sampleBuffer cap are
 *                   still visible here.
 *   2. Event ring — every DSP register write (KON/KOF/pitch/volume/...),
 *                   overflow-drop runs, and consume (dsp_getSamples)
 *                   events, all timestamped in native-sample time.
 *   3. Counters   — produced/dropped/consumed totals, producer
 *                   attribution (CPU-thread catch-up vs audio-thread
 *                   top-up), output-ring occupancy high-water, and a
 *                   once-per-second snapshot ring for rate analysis.
 *
 * All record hooks run under RtlApuLock (dsp_cycle / dsp_write /
 * dsp_getSamples are only reached with the APU lock held), so plain
 * fields suffice. Dump/query entry points take RtlApuLock themselves.
 */

#include <stdint.h>

/* 2^22 native samples ~= 131 s @ 32 kHz (16 MiB, stereo int16). */
#define AUDIO_TRACE_PCM_RING   (1u << 22)
/* DSP register writes run a few hundred per frame at most; 2^19 entries
 * (~8 MiB) holds a full multi-minute session without evicting boot. */
#define AUDIO_TRACE_EVENT_RING (1u << 19)
/* Once-per-second stat snapshots: ~68 min. */
#define AUDIO_TRACE_SNAP_RING  (1u << 12)

enum {
  AUDIO_TRACE_EV_REG     = 1, /* DSP register write: addr, val            */
  AUDIO_TRACE_EV_DROP    = 2, /* output-ring overflow run: aux = run len  */
  AUDIO_TRACE_EV_CONSUME = 3, /* dsp_getSamples: aux = avail after read   */
  /* CPU<->SPC port traffic ($2140-43 <-> $F4-F7). addr = port index 0-3,
   * aux = snes_frame_counter at event time. Together these four event
   * classes make every sound-command handoff auditable: a game sound
   * request is a CPU_PORT_WRITE; the SPC engine observing it is the
   * following SPC_PORT_READ of the same port/value; a request that is
   * overwritten by a later CPU_PORT_WRITE with no SPC_PORT_READ in
   * between was dropped before the engine ever saw it. */
  AUDIO_TRACE_EV_CPU_PORT_WRITE = 4, /* CPU wrote $2140+n (RtlApuWrite)   */
  AUDIO_TRACE_EV_SPC_PORT_READ  = 5, /* SPC read $F4+n; recorded only on
                                      * value change or first read after a
                                      * CPU write (steady-state polling of
                                      * an unchanged port is elided)      */
  AUDIO_TRACE_EV_SPC_PORT_WRITE = 6, /* SPC wrote outPort $F4+n; recorded
                                      * only on value change              */
  AUDIO_TRACE_EV_CPU_PORT_READ  = 7, /* CPU read $2140+n; recorded only on
                                      * value change or first read after an
                                      * SPC outPort write                 */
  AUDIO_TRACE_EV_CPU_PORT_APPLY = 8, /* queued CPU port write actually
                                      * landed in inPorts at its scheduled
                                      * APU-sample target (apu.c drain).
                                      * CPU_PORT_WRITE is the request;
                                      * APPLY is when the engine can see
                                      * it. */
};

/* Producer attribution for samples (who is cycling the APU). */
enum {
  AUDIO_TRACE_PRODUCER_UNKNOWN = 0,
  AUDIO_TRACE_PRODUCER_CPU     = 1, /* snes_catchupApu (CPU thread)       */
  AUDIO_TRACE_PRODUCER_AUDIO   = 2, /* RtlRenderAudio top-up (audio thread)*/
};

typedef struct AudioTraceEvent {
  uint64_t sample_idx; /* native-sample clock when the event occurred */
  uint32_t aux;        /* DROP: run length; CONSUME: ring avail after */
  uint8_t  type;
  uint8_t  addr;       /* REG only */
  uint8_t  val;        /* REG only */
  uint8_t  producer;   /* who was cycling the APU at the time */
} AudioTraceEvent;

typedef struct AudioTraceSnap {
  uint64_t wall_ms;
  uint64_t produced;
  uint64_t dropped;
  uint64_t consumed;
  uint32_t occupancy;  /* output-ring fill at snapshot time */
} AudioTraceSnap;

typedef struct AudioTraceStats {
  uint64_t produced;          /* total native samples generated         */
  uint64_t produced_cpu;      /* ... by CPU-thread catch-up             */
  uint64_t produced_audio;    /* ... by audio-thread top-up             */
  uint64_t dropped;           /* total samples lost to ring overflow    */
  uint64_t drop_runs;         /* number of distinct drop bursts         */
  uint64_t dropped_audible;   /* dropped samples carrying real signal
                               * (|l| or |r| > 256 ~= -42 dBFS). Upload-
                               * phase drops are silence and land only in
                               * `dropped`; THIS counter going nonzero
                               * means audible music was discarded.      */
  uint64_t consumed;          /* total native samples read for output   */
  uint64_t consume_calls;     /* dsp_getSamples calls (audio callbacks) */
  uint64_t reg_writes;        /* DSP register writes                    */
  uint64_t kon_writes;        /* writes to $4C (KON)                    */
  uint32_t occupancy_highwater;
  uint64_t event_count;       /* events recorded (monotonic)            */
  uint64_t snap_count;        /* snapshots recorded (monotonic)         */
  /* APU catch-up pacing (rtl_accumulate_apu_catchup):                  */
  uint64_t pace_baseline_cycles;  /* wall-clock cycles injected while   */
                                  /* no consumer was draining the ring  */
  uint64_t pace_accumulate_calls; /* catch-up accumulations (APU touches)*/
  uint32_t pace_consumer_active;  /* consumer draining at last catch-up */
  /* Port-traffic totals (appended; events themselves are in the ring). */
  uint64_t cpu_port_writes;       /* every CPU write to $2140-43        */
  uint64_t spc_port_reads_seen;   /* every SPC read of $F4-F7 (raw)     */
  uint64_t spc_port_reads_logged; /* ... of which recorded in the ring  */
  uint64_t spc_port_writes;       /* SPC outPort writes (raw)           */
  uint64_t cpu_port_reads_logged; /* CPU port reads recorded in the ring*/
  /* CPU port writes that were OVERWRITTEN by a later CPU write to the
   * same port before any SPC read observed them — the "command lost
   * before the engine saw it" counter. Per-port. */
  uint64_t cpu_port_overwrites[4];

  /* In-process DSP reference divergence (dev / SNESRECOMP_TRACE builds only).
   * Per output sample, the canon dry mix vs the dsp_shadow reference re-render,
   * in the normalized [-1,1] domain, accumulated only while the canon mix is
   * non-silent. This is the internal lockstep tone oracle: artifact-free (no
   * cross-process resample / alignment that confounds the bsnes WAV diff). The
   * current reference is the cubic-interpolation render, so the number is the
   * Gaussian-interpolation tone contribution (canon Gaussian vs ideal cubic).
   * RMS = sqrt(shadow_div_sumsq / shadow_div_count); report 20*log10(RMS) dB. */
  uint64_t shadow_div_count;   /* non-silent output samples measured        */
  double   shadow_div_sumsq;   /* sum over samples of (dL^2 + dR^2) / 2     */
  double   shadow_div_max;     /* peak |d| (max of |dL|,|dR|) over session  */

  /* FAITHFUL-reference divergence (dev / SNESRECOMP_TRACE only). Per active
   * voice per output sample: canon dsp_getSample vs blargg's snes9x/bsnes
   * reference Gaussian (same canonical gaussValues table, blargg's exact integer
   * algorithm), normalized [-1,1]. Unlike shadow_div (canon vs the cubic
   * ENHANCEMENT, a tone-character meter), this is canon vs the HARDWARE
   * reference: ~0 proves the recomp Gaussian matches snes9x/bsnes; a residual is
   * the canon >>10+>>1 vs blargg >>11 rounding difference, attributed in-process
   * (no cross-process resample). RMS = sqrt(faithful_div_sumsq/faithful_div_count). */
  uint64_t faithful_div_count;
  double   faithful_div_sumsq;
  double   faithful_div_max;

  /* BRR-decode faithful-reference divergence (dev only). Per decoded BRR sample,
   * canon dsp_decodeBrr vs blargg's snes9x/bsnes decode_brr (re-decoded from the
   * same ARAM block + seeds), compared in a common full-scale domain (canon
   * stores half-scale; blargg full-scale = 2x). ~0 proves the recomp BRR decode;
   * a residual is the shift>12 invalid-block / rounding handling. */
  uint64_t brr_div_count;
  double   brr_div_sumsq;
  double   brr_div_max;
  uint32_t brr_first_valid;
  uint16_t brr_first_block;
  uint8_t  brr_first_header;
  uint8_t  brr_first_sample;
  int32_t  brr_first_canon;
  int32_t  brr_first_reference;
  int32_t  brr_first_old;
  int32_t  brr_first_older;

  /* Echo-FIR faithful-reference divergence (dev only). Per output sample, canon
   * dsp_handleEcho 8-tap FIR sum vs blargg's CALC_FIR reference on the same FIR
   * history + coefficients, normalized [-1,1]. ~0 proves the recomp echo FIR. */
  uint64_t echo_div_count;
  double   echo_div_sumsq;
  double   echo_div_max;
} AudioTraceStats;

/* ---- record hooks (call sites: dsp.c, snes.c, common_rtl.c) ---- */
void audio_trace_on_sample(int16_t l, int16_t r, int dropped, uint32_t ring_fill);
void audio_trace_on_reg_write(uint8_t addr, uint8_t val);
void audio_trace_on_consume(uint64_t read_idx, uint32_t count, uint32_t avail_after);
void audio_trace_set_producer(int producer);
/* Record one output sample's canon-vs-reference DSP divergence (normalized
 * [-1,1] domain). Dev-only tone measurement; called from dsp_shadow under
 * RtlApuLock. Silent samples (both args ~0 with a silent canon) should be
 * filtered by the caller so the RMS reflects active audio. */
void audio_trace_on_shadow_div(double dl, double dr);
/* Record one active-voice sample's canon-vs-faithful-reference Gaussian
 * divergence (normalized [-1,1]). Dev-only; called from dsp_shadow per voice. */
void audio_trace_on_faithful_div(double d);
/* Record one BRR-decoded sample's canon-vs-reference comparison. Retains the
 * first mismatching block/header/sample as a deterministic runtime witness.
 * Dev-only; called from dsp_shadow_verify_brr. */
void audio_trace_on_brr_compare(uint16_t block, uint8_t header, uint8_t sample,
                                int canon, int reference, int old, int older);
/* Record one output sample's canon-vs-reference echo-FIR divergence (normalized
 * [-1,1], L/R averaged). Dev-only; called from dsp_handleEcho. */
void audio_trace_on_echo_div(double d);
/* Per catch-up accumulation: consumer state + wall-clock baseline cycles
 * injected (0 when a consumer is draining or no wall time elapsed). */
void audio_trace_on_pace(int consumer_active, uint32_t baseline_cycles);
/* CPU<->SPC port traffic. port = 0-3. All call sites hold RtlApuLock.
 * The SPC-read / CPU-read hooks gate internally (value change or fresh
 * counterpart write); callers pass every access unconditionally. */
void audio_trace_on_cpu_port_write(uint8_t port, uint8_t val);
void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t val);
void audio_trace_on_spc_port_read(uint8_t port, uint8_t val);
void audio_trace_on_spc_port_write(uint8_t port, uint8_t val);
void audio_trace_on_cpu_port_read(uint8_t port, uint8_t val);

/* Authoritative native-sample clocks (produced = samples the DSP has
 * emitted; consumed = samples the audio callback has read). Exported so
 * the port-write scheduler anchors its targets on the same clock the
 * trace uses. Caller must hold RtlApuLock. */
void audio_trace_sample_clocks(uint64_t *produced, uint64_t *consumed);

/* Monotonic wall-clock milliseconds — the same timebase the snapshot ring
 * uses, exported so the catch-up pacer and any analysis share one clock. */
uint64_t audio_trace_wall_ms(void);
/* Monotonic high-resolution nanoseconds (QPC / CLOCK_MONOTONIC) — for
 * sub-frame spacing of scheduled port writes; wall_ms granularity
 * (~15 ms on Windows) is coarser than a frame. */
uint64_t audio_trace_wall_ns(void);
/* APU burst granularity: the largest native-sample chunk a callback has
 * consumed (floor 534). The port-write scheduler's caps scale with this
 * so larger user-configured audio buffers can't re-collapse command
 * spacing. Caller must hold RtlApuLock. */
uint32_t audio_trace_consume_quantum(void);

/* ---- query/dump (any thread; takes RtlApuLock internally) ---- */
void audio_trace_get_stats(AudioTraceStats *out);
/* Copy events [first_idx, first_idx+max) into out; returns count copied.
 * first/oldest available index is written to *oldest. */
uint32_t audio_trace_copy_events(uint64_t first_idx, uint32_t max,
                                 AudioTraceEvent *out, uint64_t *oldest);
uint32_t audio_trace_copy_snaps(uint64_t first_idx, uint32_t max,
                                AudioTraceSnap *out, uint64_t *oldest);
/* Write a 32 kHz stereo 16-bit WAV of PCM-ring samples
 * [start_idx, start_idx+count). start_idx<0 / count==0 mean "everything
 * still in the ring". Returns 0 on success, writes the actually-dumped
 * range to *out_start/*out_count. */
int audio_trace_dump_wav(const char *path, int64_t start_idx, uint64_t count,
                         uint64_t *out_start, uint64_t *out_count);

#endif
