#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_trace.h"

int snes_frame_counter = 1234;

static int g_overwrite_on_unlock;
static uint64_t g_overwrite_samples;

void RtlApuLock(void) {}
void RtlApuUnlock(void) {
  if (!g_overwrite_on_unlock)
    return;
  g_overwrite_on_unlock = 0;
  for (uint64_t i = 0; i < g_overwrite_samples; i++) {
    int16_t l = (int16_t)(2000 + (int)(i & 0x3ff));
    audio_trace_on_sample(l, (int16_t)-l, 0, 0);
  }
}

static void fail(const char *msg) {
  fprintf(stderr, "audio_trace_history_test: %s\n", msg);
  exit(1);
}

static void expect_u64(const char *name, uint64_t got, uint64_t want) {
  if (got != want) {
    fprintf(stderr,
            "audio_trace_history_test: %s got %llu want %llu\n",
            name, (unsigned long long)got, (unsigned long long)want);
    exit(1);
  }
}

static void expect_u32(const char *name, uint32_t got, uint32_t want) {
  if (got != want) {
    fprintf(stderr, "audio_trace_history_test: %s got %u want %u\n",
            name, got, want);
    exit(1);
  }
}

#if AUDIO_TRACE_WRITES_HISTORY
static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static int16_t rd_s16(const uint8_t *p) {
  return (int16_t)rd16(p);
}

static void expect_wav_snapshot(const char *path, uint64_t sample_count) {
  static const int16_t expected[][2] = {
      {100, -100}, {300, 0}, {301, 0}, {0, 0}, {400, 0},
      {401, 0}, {0, 0}, {500, 0}, {501, 0},
  };
  uint8_t header[44];
  FILE *f = fopen(path, "rb");

  if (!f)
    fail("wav dump was not created");
  if (fread(header, 1, sizeof(header), f) != sizeof(header))
    fail("wav dump header is truncated");
  if (memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
      memcmp(header + 36, "data", 4) != 0)
    fail("wav dump header has invalid chunk ids");

  expect_u32("wav fmt size", rd32(header + 16), 16);
  expect_u32("wav format", rd16(header + 20), 1);
  expect_u32("wav channels", rd16(header + 22), 2);
  expect_u32("wav sample rate", rd32(header + 24), 32040);
  expect_u32("wav byte rate", rd32(header + 28), 32040 * 4);
  expect_u32("wav block align", rd16(header + 32), 4);
  expect_u32("wav bits", rd16(header + 34), 16);
  expect_u32("wav data bytes", rd32(header + 40), (uint32_t)(sample_count * 4));
  if (sample_count != sizeof(expected) / sizeof(expected[0]))
    fail("wav snapshot test expected table does not match sample count");

  for (uint64_t i = 0; i < sample_count; i++) {
    uint8_t sample[4];
    if (fread(sample, 1, sizeof(sample), f) != sizeof(sample))
      fail("wav dump data is truncated");
    if (rd_s16(sample) != expected[i][0] ||
        rd_s16(sample + 2) != expected[i][1])
      fail("wav dump data does not match locked snapshot");
  }
  if (fgetc(f) != EOF)
    fail("wav dump data is longer than header declares");
  if (fclose(f) != 0)
    fail("failed to close wav dump");
}
#endif

int main(void) {
  AudioTraceStats st;
  AudioTraceEvent events[16];
  char wav_path[1024];
  const char *tmp_dir = getenv("TMPDIR");
  uint64_t produced = 0;
  uint64_t consumed = 0;
  uint64_t oldest = 0;
  uint64_t wav_start = 99;
  uint64_t wav_count = 99;

  if (!tmp_dir || !tmp_dir[0])
    tmp_dir = getenv("TMP");
  if (!tmp_dir || !tmp_dir[0])
    tmp_dir = getenv("TEMP");
  if (!tmp_dir || !tmp_dir[0])
    tmp_dir = ".";
  if (snprintf(wav_path, sizeof(wav_path),
               "%s/audio_trace_history_test_%p.wav",
               tmp_dir, (void *)&st) >= (int)sizeof(wav_path))
    fail("temp wav path too long");

  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  audio_trace_on_sample(100, -100, 0, 7);
  audio_trace_on_sample(300, 0, 1, 8);
  audio_trace_on_cpu_port_write(2, 0x33);
  audio_trace_on_sample(301, 0, 1, 9);
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_AUDIO);
  audio_trace_on_sample(0, 0, 0, 2);

  audio_trace_on_reg_write(0x4c, 1);
  audio_trace_on_cpu_port_apply(2, 0x33);
  audio_trace_on_cpu_port_apply(2, 0x44);
  audio_trace_on_spc_port_read(2, 0x44);
  audio_trace_on_spc_port_write(1, 0xaa);
  audio_trace_on_cpu_port_read(1, 0xaa);

  audio_trace_on_consume(0, 640, 12);
  audio_trace_on_fast_forward_discard(3, 9);
  audio_trace_on_output_underflow(1);
  audio_trace_on_pace(1, 77);
  audio_trace_on_guest_sync(1, 1000);
  audio_trace_on_guest_sync(0, 2000);

  audio_trace_sample_clocks(&produced, &consumed);
  expect_u64("sample clock produced", produced, 4);
  expect_u64("sample clock consumed", consumed, 640);
  expect_u32("consume quantum", audio_trace_consume_quantum(), 640);

  audio_trace_get_stats(&st);
  expect_u64("produced", st.produced, 4);
  expect_u64("produced_cpu", st.produced_cpu, 3);
  expect_u64("produced_audio", st.produced_audio, 1);
  expect_u64("dropped", st.dropped, 2);
  expect_u64("drop_runs", st.drop_runs, 1);
  expect_u64("dropped_audible", st.dropped_audible, 2);
  expect_u64("consumed", st.consumed, 640);
  expect_u64("consume_calls", st.consume_calls, 1);
  expect_u64("reg_writes", st.reg_writes, 1);
  expect_u64("kon_writes", st.kon_writes, 1);
  expect_u32("occupancy_highwater", st.occupancy_highwater, 9);
  expect_u32("occupancy_current", st.occupancy_current, 1);
  expect_u64("fast_forward_discarded", st.fast_forward_discarded, 3);
  expect_u64("output_underflows", st.output_underflows, 1);
  expect_u64("pace_baseline_cycles", st.pace_baseline_cycles, 77);
  expect_u64("pace_accumulate_calls", st.pace_accumulate_calls, 1);
  expect_u32("pace_consumer_active", st.pace_consumer_active, 1);
  expect_u64("guest_frame_sync_cycles", st.guest_frame_sync_cycles, 1000);
  expect_u64("guest_read_sync_cycles", st.guest_read_sync_cycles, 2000);
  expect_u64("cpu_port_writes", st.cpu_port_writes, 1);
  expect_u64("spc_port_reads_seen", st.spc_port_reads_seen, 1);
  expect_u64("spc_port_reads_logged", st.spc_port_reads_logged, 1);
  expect_u64("spc_port_writes", st.spc_port_writes, 1);
  expect_u64("cpu_port_reads_logged", st.cpu_port_reads_logged, 1);
  expect_u64("cpu_port_overwrites[2]", st.cpu_port_overwrites[2], 1);

  if (st.event_count != 9)
    fail("event_count did not preserve event/drop-run accounting");

  {
    uint32_t copied = audio_trace_copy_events(0, 16, events, &oldest);
#if AUDIO_TRACE_WRITES_HISTORY
    expect_u32("copied events", copied, 9);
    expect_u64("oldest event", oldest, 0);
    if (events[0].type != AUDIO_TRACE_EV_DROP || events[0].aux != 2)
      fail("drop event history did not coalesce");
#else
    expect_u32("copied events", copied, 0);
    expect_u64("oldest event", oldest, st.event_count);
#endif
  }

  {
    const uint64_t before_events = st.event_count;
    const uint64_t before_runs = st.drop_runs;
    const uint64_t before_dropped = st.dropped;
    const uint64_t before_cpu_port_writes = st.cpu_port_writes;
    const uint32_t interleaved_events =
        AUDIO_TRACE_DROP_RUN_EVENT_RING - 1u;

    audio_trace_on_sample(400, 0, 1, 10);
    for (uint32_t i = 0; i < interleaved_events; i++)
      audio_trace_on_cpu_port_write((uint8_t)i, (uint8_t)(i + 1));
    audio_trace_on_sample(401, 0, 1, 11);

    audio_trace_get_stats(&st);
    expect_u64("capacity drop-run count", st.drop_runs, before_runs + 1);
    expect_u64("capacity drop sample count", st.dropped, before_dropped + 2);
    expect_u64("capacity drop event count", st.event_count,
               before_events + AUDIO_TRACE_DROP_RUN_EVENT_RING);
    expect_u64("capacity CPU port writes", st.cpu_port_writes,
               before_cpu_port_writes + interleaved_events);

#if AUDIO_TRACE_WRITES_HISTORY
    {
      uint32_t copied = audio_trace_copy_events(before_events, 1,
                                                events, &oldest);
      expect_u32("capacity first event copy", copied, 1);
      if (events[0].type != AUDIO_TRACE_EV_DROP || events[0].aux != 2)
        fail("capacity boundary did not extend retained DROP event");
    }
#endif
  }

  audio_trace_on_sample(0, 0, 0, 0);

  {
    const uint64_t before_events = st.event_count;
    const uint64_t before_runs = st.drop_runs;
    const uint64_t before_dropped = st.dropped;
    const uint64_t before_cpu_port_writes = st.cpu_port_writes;
    const uint32_t interleaved_events = AUDIO_TRACE_DROP_RUN_EVENT_RING;

    audio_trace_on_sample(500, 0, 1, 12);
    for (uint32_t i = 0; i < interleaved_events; i++)
      audio_trace_on_cpu_port_write((uint8_t)i, (uint8_t)(i + 3));
    audio_trace_on_sample(501, 0, 1, 13);

    audio_trace_get_stats(&st);
    expect_u64("capacity+1 drop-run count", st.drop_runs, before_runs + 2);
    expect_u64("capacity+1 drop sample count", st.dropped,
               before_dropped + 2);
    expect_u64("capacity+1 drop event count", st.event_count,
               before_events + AUDIO_TRACE_DROP_RUN_EVENT_RING + 2);
    expect_u64("capacity+1 CPU port writes", st.cpu_port_writes,
               before_cpu_port_writes + interleaved_events);

#if AUDIO_TRACE_WRITES_HISTORY
    {
      uint32_t copied = audio_trace_copy_events(st.event_count - 1, 1,
                                                events, &oldest);
      expect_u32("capacity+1 last event copy", copied, 1);
      if (events[0].type != AUDIO_TRACE_EV_DROP || events[0].aux != 1)
        fail("capacity+1 boundary did not create retained DROP fragment");
    }
#endif
  }

  {
    int wav_rc;
    wav_start = 99;
    wav_count = 99;
    wav_rc = audio_trace_dump_wav("", 0, 1, &wav_start, &wav_count);
    if (wav_rc == 0)
      fail("wav dump should fail for an empty path");
    expect_u64("empty-path wav_start", wav_start, 0);
    expect_u64("empty-path wav_count", wav_count, 0);
  }

  {
    int wav_rc;
    wav_start = 99;
    wav_count = 99;
    wav_rc = audio_trace_dump_wav(wav_path, -1, 0, &wav_start, &wav_count);
#if AUDIO_TRACE_WRITES_HISTORY
    if (wav_rc != 0)
      fail("wav dump should succeed when PCM history is written");
    expect_u64("wav_start", wav_start, 0);
    expect_u64("wav_count", wav_count, 9);
    expect_wav_snapshot(wav_path, wav_count);
    remove(wav_path);
#else
    if (wav_rc == 0)
      fail("wav dump should fail when PCM history is unavailable");
    expect_u64("wav_start", wav_start, 0);
    expect_u64("wav_count", wav_count, 0);
#endif
  }

#if SNESRECOMP_AUDIO_TRACE_HISTORY == AUDIO_TRACE_HISTORY_SMALL
  {
    int wav_rc;
    g_overwrite_samples = AUDIO_TRACE_PCM_RING + 16u;
    g_overwrite_on_unlock = 1;
    wav_rc = audio_trace_dump_wav(wav_path, 0, 9, &wav_start, &wav_count);
    if (wav_rc != 0)
      fail("small wav snapshot dump should survive producer overwrite");
    expect_u64("small wav_start", wav_start, 0);
    expect_u64("small wav_count", wav_count, 9);
    expect_wav_snapshot(wav_path, wav_count);
    remove(wav_path);
  }
#endif

  printf("mode=%d sizeof(AudioTraceEvent)=%zu sizeof(AudioTraceSnap)=%zu "
         "pcm_ring=%u event_ring=%u snap_ring=%u writes_history=%d\n",
         SNESRECOMP_AUDIO_TRACE_HISTORY,
         sizeof(AudioTraceEvent), sizeof(AudioTraceSnap),
         AUDIO_TRACE_PCM_RING, AUDIO_TRACE_EVENT_RING, AUDIO_TRACE_SNAP_RING,
         AUDIO_TRACE_WRITES_HISTORY ? 1 : 0);
  return 0;
}
