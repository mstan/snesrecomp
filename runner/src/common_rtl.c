#include "common_rtl.h"
#include "common_cpu_infra.h"
#include <setjmp.h>
#include <time.h>
#include <stdlib.h>
#include "recomp_hw.h"
#include "framedump.h"
#include "util.h"
#include "config.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/msu1.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "audio_trace.h"
#include "ppu_dma_trace.h"
#include "host_report.h"
#include "cosim.h"

uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;
uint8 g_snesrecomp_last_hdmaen;

// Main-CPU cycle estimate, incremented per RDB_BLOCK_HOOK in debug_on_block_enter.
// Used to pace APU catchup realistically: real SNES is ~3.58 MHz main / ~1.024 MHz APU,
// ratio ~3.5:1. Prior code hardcoded apuCatchupCycles=32 per APU port touch regardless
// of elapsed main-CPU time, which let APU stay artificially synchronized -- SMW boot's
// "wait for APU ack" loops resolved instantly, racing through ~200 frames worth of game
// logic in ~95 frames. Tracking real elapsed cycles makes those waits actually wait.
uint64_t g_main_cpu_cycles_estimate = 0;
// APU-port-touch-only estimate (issue #4). Only touches of $2140-$217F
// count toward APU catch-up pacing. The general estimate above counts
// EVERY HW-reg touch, which wildly over-counts during load-heavy phases:
// graphics decompression hammers $2118/$2122 thousands of times per
// frame, and the next APU-port access used to convert that entire
// fictional backlog into SPC cycles at once. Measured on MMX (audio_trace
// rings, 300 s boot→attract): 237,426 native samples — 7.4 s of SPC
// over-advance — dropped at the output ring, clustered at scene
// transitions, audibly skipping the music forward at every stage load.
// Genuine APU handshakes (boot/bank uploads, command acks) self-pace
// through their own port polls — each poll is an APU touch — so they
// still over-clock the SPC and complete fast; that part is required:
// at hardware-real SPC pace MMX's boot upload blocks a single frame
// for >5 s and trips the per-frame watchdog (measured, 2026-06-09).
uint64_t g_apu_pace_cycles_estimate = 0;
uint64_t g_apu_last_sync_cycles = 0;

// Set while the interp816 bridge (interp tier / LLE task scheduler) is driving:
// the bridge advances the SPC itself, per interpreted opcode, by guest master
// cycles (like the faithful oracle) — so the per-touch synthetic catch-up must
// NOT also fire (double-count). Guards rtl_accumulate_apu_catchup below. Only
// the interp path sets it; the compiled steady-state path is unaffected.
int g_interp_apu_driving = 0;

#ifdef SNES_COSIM
/* Shared APU clock knob (common_rtl.h). Cached once; both A/B processes of a
 * same-binary pair must be launched with the same value. */
int cosim_apu_shared_clock(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("SNES_COSIM_APU_SHARED");
    v = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  return v;
}
#endif

// Axis-5 off-cue: APU pacing now derives from the recompiler's region-weighted
// MASTER-clock accumulator (g_cpu.master_cycles) instead of the +256-per-touch
// synthetic estimate. g_apu_last_sync_master is the master-clock count at the
// last APU sync; rtl_accumulate_apu_catchup() converts the delta to SPC cycles.
uint64_t g_apu_last_sync_master = 0;
// $420D bit 0 (FastROM / MEMSEL): 1 => $80-$FF:$8000-$FFFF code runs fast (6
// master clocks/access) instead of slow (8). Tracked here so emitted blocks in
// the WS2 mirror banks weight their master-cycle charge correctly at runtime.
// SlowROM games (e.g. SMW) never set it and run all code from $00-$3F (speed is
// memsel-independent there), so this stays 0 and the emitted charge is constant.
uint8_t g_memsel = 0;

// Axis-7 determinism: always-on per-frame WRAM fingerprint ring. FNV-1a of the
// full 128KB g_ram each frame, keyed by frame number. Two runs from the same
// reset produce identical sequences iff the recompiler is deterministic — which
// every diff loop (audio / PPU / cycle) silently presupposes. Cheap (~128K hash
// ops/frame). Dumped on demand via the debug server (`fingerprint`). FP_RING is
// defined in common_rtl.h.
uint64_t g_fp_ring[FP_RING];
uint64_t g_fp_max_frame;
static uint64_t fp_fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

// FILE-backed SaveLoadInfo. snes_saveload calls back into func() once per
// scalar/blob; we route each call to fread/fwrite. Single magic+version
// header lets future format changes be detected.
#define RTL_SAV_MAGIC   0x52544c53u  /* "RTLS" */
/* v4: dropped Dma.pad[7] blob tail.
 * v5: optional game-specific chunk appended after the snes_saveload blob
 *     (RtlGameInfo.state_save_extra/state_load_extra) — e.g. MMX task-slot
 *     resume contexts so a load can rebuild its scheduler fibers from any
 *     game mode or a fresh process. v4 files still load (no chunk). */
#define RTL_SAV_VERSION 5u
#define RTL_SAV_VERSION_MIN 4u

typedef struct FileSli {
  SaveLoadInfo base;
  FILE *f;
  bool is_save;
  bool error;
} FileSli;

typedef struct MemorySli {
  SaveLoadInfo base;
  uint8 *data;
  size_t capacity;
  size_t position;
  bool is_save;
  bool error;
} MemorySli;

static void file_sli_func(SaveLoadInfo *sli, void *data, size_t n) {
  FileSli *fs = (FileSli *)sli;
  if (fs->error) return;
  size_t got = fs->is_save ? fwrite(data, 1, n, fs->f)
                           : fread(data, 1, n, fs->f);
  if (got != n) fs->error = true;
}

static void memory_sli_func(SaveLoadInfo *sli, void *data, size_t n) {
  MemorySli *memory = (MemorySli *)sli;
  if (memory->error || n > SIZE_MAX - memory->position) {
    memory->error = true;
    return;
  }
  if (memory->data) {
    if (memory->position + n > memory->capacity) {
      memory->error = true;
      return;
    }
    if (memory->is_save)
      memcpy(memory->data + memory->position, data, n);
    else
      memcpy(data, memory->data + memory->position, n);
  } else if (!memory->is_save) {
    memory->error = true;
    return;
  }
  memory->position += n;
}

void RtlReset(int mode) {
  snes_frame_counter = 0;
  g_main_cpu_cycles_estimate = 0;
  g_apu_pace_cycles_estimate = 0;
  g_apu_last_sync_cycles = 0;
  // master_cycles is monotonic across soft reset (RtlReset doesn't re-init
  // g_cpu); anchor the sync pointer to its current value so the first post-reset
  // catch-up sees a zero delta rather than the whole run's accumulated cycles.
  g_apu_last_sync_master = g_cpu.master_cycles;
  snes_reset(g_snes, true);
  g_snes->beamMasterLast = g_cpu.master_cycles;
  SnesEnterNativeMode();
  ppu_reset(g_ppu);
  if (!(mode & 1))
    memset(g_sram, 0, g_sram_size);

  RtlApuLock();
  g_spc_player->initialize(g_spc_player);
  RtlApuUnlock();
}

/* Differential first-divergence trace (docs/MULTI_TIER.md §12a). Env-gated,
 * always-compiled, zero cost when off. Emits per-frame changed bytes of low
 * WRAM ($0000-$1FFF) in the EXACT jsonl shape snesref produces, so the two
 * traces diff directly. Sampled at end-of-frame (after run_frame), mirroring
 * snesref's post-retro_run trace_tick; frame numbering starts at 1 on the
 * first completed frame, matching snesref, so frames align by construction. */
static void recomp_wram_trace_tick(void) {
    static int enabled = -1;
    static FILE *log = NULL;
    static unsigned char prev[0x2000];
    static int primed = 0;
    static unsigned frame = 0;
    if (enabled < 0) {
        const char *p = getenv("SNESRECOMP_WRAM_TRACE_FILE");
        enabled = (p && p[0]) ? 1 : 0;
    }
    if (!enabled) return;
    if (!log) {
        log = fopen(getenv("SNESRECOMP_WRAM_TRACE_FILE"), "a");
        if (!log) { enabled = 0; return; }
    }
    frame++;
    if (!primed) {
        for (int a = 0; a <= 0x1fff; a++) {
            prev[a] = g_ram[a];
            fprintf(log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x00\",\"val\":\"0x%02x\"}\n",
                    frame, a, g_ram[a]);
        }
        primed = 1; return;
    }
    for (int a = 0; a <= 0x1fff; a++) {
        unsigned char v = g_ram[a];
        if (v != prev[a]) {
            fprintf(log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                    frame, a, prev[a], v);
            prev[a] = v;
        }
    }
    if ((frame % 30) == 0) fflush(log);
}

/* APU/SPC-RAM variant of the differential trace (co-sim step 1: audio hunt).
 * Emits per-frame changed bytes of the full 64K SPC RAM (g_snes->apu->ram) in
 * the SAME jsonl shape as the WRAM trace, so it aligns 1:1 against snesref's
 * bsnes SPC-RAM trace (retro memory id 0x100) via align_diff.py --size 0x10000.
 * The recomp runs a real SPC700 core (LakeSnes-derived apu/spc/dsp); only the
 * IPL upload handshake is HLE'd, so apu->ram reflects the real uploaded SPC
 * program plus the SPC700's runtime writes -- directly comparable to bsnes's
 * dsp.apuram. Env-gated (SNESRECOMP_APURAM_TRACE_FILE), zero cost when off. */
static void recomp_apuram_trace_tick(void) {
    static int enabled = -1;
    static FILE *log = NULL;
    static unsigned char prev[0x10000];
    static int primed = 0;
    static unsigned frame = 0;
    extern Snes *g_snes;
    if (enabled < 0) {
        const char *p = getenv("SNESRECOMP_APURAM_TRACE_FILE");
        enabled = (p && p[0]) ? 1 : 0;
    }
    if (!enabled) return;
    if (!g_snes || !g_snes->apu) return;
    if (!log) {
        log = fopen(getenv("SNESRECOMP_APURAM_TRACE_FILE"), "a");
        if (!log) { enabled = 0; return; }
    }
    const unsigned char *ram = g_snes->apu->ram;
    frame++;
    if (!primed) {
        for (int a = 0; a <= 0xffff; a++) {
            prev[a] = ram[a];
            fprintf(log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x00\",\"val\":\"0x%02x\"}\n",
                    frame, a, ram[a]);
        }
        primed = 1; return;
    }
    for (int a = 0; a <= 0xffff; a++) {
        unsigned char v = ram[a];
        if (v != prev[a]) {
            fprintf(log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                    frame, a, prev[a], v);
            prev[a] = v;
        }
    }
    if ((frame % 30) == 0) fflush(log);
}

/* S-DSP register-file variant of the differential trace (co-sim audio hunt).
 * Emits per-frame changed bytes of the 128-byte DSP register file
 * (g_snes->apu->dsp->ram, the $00-$7F mirror incl. VxPITCHL/H, KON/KOFF, ADSR,
 * echo). Same jsonl shape as the WRAM/APU-RAM traces => aligns 1:1 against
 * snesref's bsnes DSP-reg trace (retro memory id 0x101) via align_diff.py
 * --size 0x80. This is the state where "pitch" literally lives, so a persistent
 * divergence here (esp. VxPITCH) localizes an off-pitch bug to driver-writes vs
 * DSP synthesis. Env-gated (SNESRECOMP_DSPREG_TRACE_FILE). */
static void recomp_dspreg_trace_tick(void) {
    static int enabled = -1;
    static FILE *log = NULL;
    static unsigned char prev[0x80];
    static int primed = 0;
    static unsigned frame = 0;
    extern Snes *g_snes;
    if (enabled < 0) {
        const char *p = getenv("SNESRECOMP_DSPREG_TRACE_FILE");
        enabled = (p && p[0]) ? 1 : 0;
    }
    if (!enabled) return;
    if (!g_snes || !g_snes->apu || !g_snes->apu->dsp) return;
    if (!log) {
        log = fopen(getenv("SNESRECOMP_DSPREG_TRACE_FILE"), "a");
        if (!log) { enabled = 0; return; }
    }
    const unsigned char *r = g_snes->apu->dsp->ram;
    frame++;
    if (!primed) {
        for (int a = 0; a < 0x80; a++) {
            prev[a] = r[a];
            fprintf(log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x00\",\"val\":\"0x%02x\"}\n",
                    frame, a, r[a]);
        }
        primed = 1; return;
    }
    for (int a = 0; a < 0x80; a++) {
        unsigned char v = r[a];
        if (v != prev[a]) {
            fprintf(log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                    frame, a, prev[a], v);
            prev[a] = v;
        }
    }
    if ((frame % 30) == 0) fflush(log);
}

/* Native DSP output-stream capture (co-sim audio hunt). Reads the samples the
 * S-DSP PRODUCED this frame straight from dsp->sampleBuffer (the always-on ring,
 * per CLAUDE.md ring-buffer rule) using the monotonic sampleWrite counter, and
 * appends them as raw s16 stereo PCM (~32040 Hz, pre-host-resample) — the exact
 * game-agnostic audio the recomp generates, to align against bsnes's
 * BSNES_COSIM_DSPOUT stream. Env-gated (SNESRECOMP_DSPOUT). */
static void recomp_dspout_capture(void) {
    static int enabled = -1;
    static FILE *f = NULL;
    static uint32_t prev_write = 0;
    static int primed = 0;
    extern Snes *g_snes;
    if (enabled < 0) {
        const char *p = getenv("SNESRECOMP_DSPOUT");
        enabled = (p && p[0]) ? 1 : 0;
    }
    if (!enabled) return;
    if (!g_snes || !g_snes->apu || !g_snes->apu->dsp) return;
    if (!f) { f = fopen(getenv("SNESRECOMP_DSPOUT"), "wb"); if (!f) { enabled = 0; return; } }
    Dsp *dsp = g_snes->apu->dsp;
    uint32_t w = dsp->sampleWrite;
    if (!primed) { prev_write = w; primed = 1; return; }
    for (uint32_t s = prev_write; s != w; s++) {
        uint32_t idx = (s & (DSP_SAMPLE_RING - 1)) * 2;
        int16 pair[2] = { dsp->sampleBuffer[idx], dsp->sampleBuffer[idx + 1] };
        fwrite(pair, sizeof(int16), 2, f);
    }
    prev_write = w;
}

bool RtlRunFrame(uint32 inputs) {
#ifdef SNES_COSIM
  /* Co-sim (dev/diagnostics only): connect the coordinator once, before the
   * first frame executes. Boot ran deterministically already; the co-sim
   * compares from frame 1 onward. */
  { static int s_cosim_started = 0;
    if (!s_cosim_started) { s_cosim_started = 1; cosim_init(); } }
#endif
  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;
  // Player2
  if ((inputs & 0x30000) == 0x30000) inputs ^= 0x30000;
  if ((inputs & 0xc0000) == 0xc0000) inputs ^= 0xc0000;

  g_snes->input1_currentState = inputs & 0xfff;
  g_snes->input2_currentState = (inputs >> 12) & 0xfff;

  WatchdogFrameStart();
  // Watchdog guard: WatchdogCheck() (called per-block in v2 gen) longjmps
  // here when a frame exceeds 5s, so an infinite loop in recompiled code
  // doesn't freeze the runtime indefinitely. Without this setjmp the
  // longjmp would dereference an uninitialized jmp_buf and crash.
  if (setjmp(g_watchdog_jmp) == 0) {
    g_rtl_game_info->run_frame();
  }
#ifdef SNES_COSIM
  /* DETERMINISTIC AUDIO CONSUMER (SNES_COSIM_AUDIO=1). Production pins SPC tempo
   * to the audio device's consumption rate: RtlRenderAudio cycles the SPC only
   * for the shortfall, then drains a block — so the *consumer* sets the rate.
   * Headless has no audio thread, so the SPC starves (~40x slow). Here we MODEL
   * the consumer deterministically: drain one frame of samples at the exact SNES
   * rate (32040 / 60.0988 = 533.12 samples/frame). This paces the SPC to the
   * correct tempo, reproduces the producer(CPU-catchup)/consumer(this) dynamics
   * incl. any ring drops, and stays fully deterministic (no host thread). The
   * dev dump's samples= then reflects real production audio behaviour. */
  {
    static int s_audio = -1;
    if (s_audio < 0) { const char *e = getenv("SNES_COSIM_AUDIO");
                       s_audio = (e && e[0] && e[0] != '0') ? 1 : 0; }
    if (s_audio) {
      static double s_acc = 0.0;
      static int16 s_buf[1024 * 2];
      s_acc += 32040.0 / 60.0988;           /* SNES native audio samples per frame */
      int want = (int)s_acc; s_acc -= (double)want;
      while (want > 0) {
        int chunk = want > 1024 ? 1024 : want;
        RtlRenderAudio(s_buf, chunk, 2);     /* self-balances SPC production to demand */
        want -= chunk;
      }
    }
  }
#endif
#ifdef SNES_COSIM
  /* Co-sim fidelity (env-gated for A/B): production's main loop calls
   * draw_ppu_frame() AFTER run_frame() every frame — PPU line render + HDMA +
   * raster-IRQ simulation. The headless harness omits it, so the raster IRQ
   * never fires and IRQ-managed guest state diverges from the oracle (e.g. MMX's
   * $0BA0 raster-ack flag stays governed only by the bootstrap clear). Running it
   * here makes the co-sim model the full production frame; the per-frame trace/
   * hash below then capture post-IRQ state, matching bsnes's frame boundary. */
  { static int s_draw = -1;
    if (s_draw < 0) { const char *e = getenv("SNES_COSIM_DRAW_PPU");
                      /* Default ON (faithful full production frame); opt-out with =0. */
                      s_draw = (e && e[0] == '0') ? 0 : 1; }
    if (s_draw && g_rtl_game_info && g_rtl_game_info->draw_ppu_frame)
      g_rtl_game_info->draw_ppu_frame();
  }
#endif
  // If g_watchdog_tripped is set, frame was abandoned mid-execution;
  // continue to the next frame so the user can interrupt cleanly.
  if (g_framedump_callback)
    g_framedump_callback(snes_frame_counter, g_ram);
  {
    extern void debug_server_record_frame(int);
    debug_server_record_frame(snes_frame_counter);
  }

  /* Always-on PPU/DMA observability: snapshot the live PPU once per frame
   * (forced-blank/brightness, screen-enable, CGRAM/VRAM occupancy). */
  ppudma_frame_snapshot(snes_frame_counter);

  recomp_wram_trace_tick();   /* differential first-divergence trace (env-gated) */
  recomp_apuram_trace_tick(); /* APU/SPC-RAM differential trace (audio hunt, env-gated) */
  recomp_dspreg_trace_tick(); /* S-DSP register-file differential trace (env-gated) */
  recomp_dspout_capture();    /* native DSP output-stream capture (env-gated) */

  /* Axis-7 determinism fingerprint: hash the full WRAM for this frame. */
  g_fp_ring[snes_frame_counter & (FP_RING - 1)] = fp_fnv1a(g_ram, sizeof(g_ram));
  g_fp_max_frame = (uint64_t)snes_frame_counter;

  snes_frame_counter++;

#ifdef SNES_COSIM
  /* Frame-keyed checkpoint: snapshot full state + park for the coordinator. */
  cosim_frame();
#endif

  /* Axis-2 soak instrumentation: env-gated FPS heartbeat to stderr. Counts
   * frames completed per wall-clock second (the frame loop caps at ~60 fps, so
   * ~60 = full speed; a sustained dip = slowdown). Zero cost when off; never
   * pauses the runtime (RULE 0). Enable with SNESRECOMP_FPS=1. */
  if (getenv("SNESRECOMP_FPS")) {
    static long s_last_sec = 0;
    static int  s_frames = 0;
    long now = (long)time(NULL);
    s_frames++;
    if (s_last_sec == 0) {
      s_last_sec = now;
    } else if (now != s_last_sec) {
      fprintf(stderr, "[fps] %d fps (frame=%d)\n", s_frames, snes_frame_counter);
      s_frames = 0;
      s_last_sec = now;
    }
  }

  return false;
}

void RtlSaveSnapshot(const char *filename) {
  FILE *f = fopen(filename, "wb");
  if (!f) {
    printf("Failed fopen for save: %s\n", filename);
    return;
  }
  uint32 hdr[2] = { RTL_SAV_MAGIC, RTL_SAV_VERSION };
  fwrite(hdr, sizeof(hdr), 1, f);
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, true, false };
  snes_saveload(g_snes, &fs.base);
  /* v5: game-specific chunk (task-slot resume contexts etc.). Streamed
   * through the same FileSli so the format stays one linear blob. */
  if (g_rtl_game_info && g_rtl_game_info->state_save_extra)
    g_rtl_game_info->state_save_extra(&fs.base);
  RtlApuUnlock();
  if (fs.error) printf("Save write error: %s\n", filename);
  fclose(f);
}

bool RtlLoadSnapshot(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (!f)
    return false;
  uint32 hdr[2];
  if (fread(hdr, sizeof(hdr), 1, f) != 1
      || hdr[0] != RTL_SAV_MAGIC
      || hdr[1] < RTL_SAV_VERSION_MIN || hdr[1] > RTL_SAV_VERSION) {
    printf("Save file %s: bad magic/version (legacy StateRecorder format no longer supported)\n", filename);
    fclose(f);
    return false;
  }
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, false, false };
  snes_saveload(g_snes, &fs.base);
  /* v5+: optional game-specific chunk follows the guest blob. Only call
   * state_load_extra when trailing bytes remain — older v5 files (and any
   * game that leaves the hook unset) have none. v4 files never have a
   * chunk; on_state_loaded still runs so the game can fall back. */
  if (hdr[1] >= 5 && g_rtl_game_info && g_rtl_game_info->state_load_extra) {
    long pos = ftell(f);
    if (pos >= 0 && fseek(f, 0, SEEK_END) == 0) {
      long end = ftell(f);
      if (fseek(f, pos, SEEK_SET) == 0 && end > pos)
        g_rtl_game_info->state_load_extra(&fs.base, hdr[1]);
    }
  }
  RtlApuUnlock();
  fclose(f);
  if (fs.error) {
    printf("Save read error: %s\n", filename);
    return false;
  }
  /* Post-load reconciliation: host-side execution state (fibers, HLE
   * scheduler bookkeeping) cannot live in the guest snapshot; give the
   * game one hook to rebuild it against the freshly restored WRAM. */
  if (g_rtl_game_info && g_rtl_game_info->on_state_loaded)
    g_rtl_game_info->on_state_loaded(hdr[1]);
  return true;
}

size_t RtlSaveSnapshotToMemory(void *data, size_t capacity) {
  MemorySli memory = {
    { &memory_sli_func }, (uint8 *)data, capacity, 0, true, false
  };
  uint32 hdr[2] = { RTL_SAV_MAGIC, RTL_SAV_VERSION };
  memory_sli_func(&memory.base, hdr, sizeof hdr);
  RtlApuLock();
  snes_saveload(g_snes, &memory.base);
  if (g_rtl_game_info && g_rtl_game_info->state_save_extra)
    g_rtl_game_info->state_save_extra(&memory.base);
  RtlApuUnlock();
  return memory.error ? 0 : memory.position;
}

bool RtlLoadSnapshotFromMemory(const void *data, size_t size) {
  if (!data || size < sizeof(uint32) * 2) return false;
  uint32 hdr[2];
  memcpy(hdr, data, sizeof hdr);
  if (hdr[0] != RTL_SAV_MAGIC || hdr[1] < RTL_SAV_VERSION_MIN ||
      hdr[1] > RTL_SAV_VERSION)
    return false;

  MemorySli memory = {
    { &memory_sli_func }, (uint8 *)data, size, sizeof hdr, false, false
  };
  RtlApuLock();
  snes_saveload(g_snes, &memory.base);
  if (hdr[1] >= 5 && g_rtl_game_info && g_rtl_game_info->state_load_extra)
    g_rtl_game_info->state_load_extra(&memory.base, hdr[1]);
  RtlApuUnlock();
  if (memory.error) return false;
  if (g_rtl_game_info && g_rtl_game_info->on_state_loaded)
    g_rtl_game_info->on_state_loaded(hdr[1]);
  return true;
}

void RtlSaveLoad(int cmd, int slot) {
  char name[128];
  const char *prefix = g_rtl_game_info->save_name_prefix;
  if (prefix)
    sprintf(name, "saves/%s%d.sav", prefix, slot);
  else
    sprintf(name, "saves/%s_save%d.sav", g_rtl_game_info->title, slot);
  printf("*** %s slot %d: %s\n",
    cmd == kSaveLoad_Save ? "Saving" : "Loading", slot, name);
  /* Breadcrumb the operation: a crash shortly after a state load is a
   * different bug class than a cold-boot crash, and the field report
   * needs to distinguish them without the user remembering. */
  host_report_breadcrumb("savestate %s: %s",
      cmd == kSaveLoad_Save ? "save" : "load", name);
  if (cmd == kSaveLoad_Save)
    RtlSaveSnapshot(name);
  else
    RtlLoadSnapshot(name);
}


void MemCpy(void *dst, const void *src, int size) {
  memcpy(dst, src, size);
}

bool Unreachable(void) {
  printf("Unreachable!\n");
  assert(0);
  g_ram[0x1ffff] = 1;
  return false;
}

uint8 *RomPtr(uint32_t addr) {
  uint8_t bank = (uint8_t)(addr >> 16);
  uint16_t lo = (uint16_t)addr;
  extern Snes *g_snes;
  uint8_t *mapped = g_snes && g_snes->cart
      ? cart_getRomPtr(g_snes->cart, bank, lo) : NULL;
  if (bank == 0x7e || bank == 0x7f || !mapped) {
    if (!g_fail) {
      const char *verbose = getenv("SNESRECOMP_OFFRAILS_STDERR");
      if (verbose && verbose[0] && verbose[0] != '0') {
        extern const char *g_last_recomp_func;
        fprintf(stderr,
                "[off-rails-romptr] addr=$%06X PB=$%02X DB=$%02X "
                "S=$%04X func=%s\n",
                (unsigned)(addr & 0xFFFFFFu), g_cpu.PB, g_cpu.DB, g_cpu.S,
                g_last_recomp_func ? g_last_recomp_func : "<none>");
      }
      g_fail = true;
    }
    /* No printf — the ring buffer + cpu_trace_offrails is the
     * channel for backwards investigation. printf'ing every bad
     * read floods stderr with millions of identical lines. */
    cpu_trace_offrails("RomPtr-invalid", addr);
  }
  /* Resolve mapping and actual-size mirroring through Cart. This prevents a
   * valid-looking guest address from becoming an out-of-allocation host read. */
  return mapped ? mapped : (uint8 *)&g_rom[0];
}

// MVN/MVP block-move pointer: resolve (bank, addr) through the cartridge map.
// Banks $00-$3F and $80-$BF mirror WRAM at $0000-$1FFF; $7E/$7F are WRAM.
// Everything else is ROM (same mapping as RomPtr). Returns a non-const pointer
// because MVN dst writes through this; callers must only dst into WRAM banks.
uint8 *MvnPtr(uint8_t bank, uint16_t addr) {
  if (bank == 0x7E) return g_ram + addr;
  if (bank == 0x7F) return g_ram + 0x10000 + addr;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && addr < 0x2000)
    return g_ram + addr;
  uint8_t *mapped = g_snes && g_snes->cart
      ? cart_getRomPtr(g_snes->cart, bank, addr) : NULL;
  if (mapped) return mapped;
  cpu_trace_offrails("MvnPtr-invalid", ((uint32_t)bank << 16) | addr);
  return (uint8 *)&g_rom[0];
}

// Replay a DMA transfer into g_ppu after the emulator executed it into g_snes->ppu.

static int _writereg_ppu_count = 0;
static int _writereg_dma_count = 0;
void WriteReg(uint16 reg, uint8 value) {
  // Direct dispatch — bypass emulator bus
  // MSU-1 ($2000-$2007). Inert unless a pack is armed, so the open-bus
  // default (no-op + log) is preserved byte-for-byte when disabled.
  if (reg >= 0x2000 && reg < 0x2008) {
    if (msu1_enabled()) msu1_write(reg, value);
    debug_server_on_reg_write(reg, value);
    return;
  }
  if (reg >= 0x3000 && reg < 0x3300 && g_snes->cart->type == CART_SUPERFX) {
    /* AOT code reaches the GSU through this direct-register fast path rather
     * than the normal CPU bus.  Bring the coprocessor up to the CPU's current
     * master clock before either side observes or changes its registers. */
    cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
    cart_write(g_snes->cart, 0, reg, value);
  } else if (reg >= 0x2100 && reg < 0x2140) {
    ppu_write(g_ppu, reg & 0xff, value);
  } else if (reg >= 0x2140 && reg < 0x2180) {
    RtlApuWrite(reg, value);
  } else if (reg >= 0x2180 && reg < 0x2184) {
    snes_writeBBus(g_snes, reg & 0xff, value);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    if (reg == 0x420C)
      g_snesrecomp_last_hdmaen = value;
    if (reg == 0x420D)
      g_memsel = (uint8_t)(value & 1);  /* FastROM select; paces $80-FF code */
    recomp_write_internal_reg(reg, value);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    dma_write(g_dma, reg, value);
  }
  debug_server_on_reg_write(reg, value);
}


uint8 ReadReg(uint16 reg) {
  // Direct dispatch — bypass emulator bus
  // MSU-1 ($2000-$2007). Returns 0 (open bus) when no pack is armed,
  // matching the prior fall-through behaviour exactly.
  if (reg >= 0x2000 && reg < 0x2008) {
    return msu1_enabled() ? msu1_read(reg) : 0;
  }
  if (reg == 0x2137)
    snes_sync_master_clock(g_snes, g_cpu.master_cycles);
  if (reg >= 0x3000 && reg < 0x3300 && g_snes->cart->type == CART_SUPERFX) {
    cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
    return cart_read(g_snes->cart, 0, reg);
  }
  if (reg >= 0x2100 && reg < 0x2140) {
    return ppu_read(g_ppu, reg & 0xff);
  } else if (reg >= 0x2140 && reg < 0x2180) {
    // APU read — route through emulator (real SPC700 outPorts).
    return snes_read(g_snes, reg);
  } else if (reg == 0x2180) {
    return snes_readBBus(g_snes, reg & 0xff);
  } else if (reg == 0x4016 || reg == 0x4017) {
    /* JOYSER0 / JOYSER1 — manual joypad-read serial registers.
     * Routed through snes_readReg so the SNES core can return the
     * controller-presence signature (bit 0 set after the implicit
     * "16 reads done" state). Phase B koopa-stomp investigation
     * (2026-04-24) found these reads were falling through to the
     * default `return 0` and breaking SMW's CheckWhichControllers-
     * ArePluggedIn detection. */
    return snes_readReg(g_snes, reg);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    return recomp_read_internal_reg(reg);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    return dma_read(g_dma, reg);
  }
  return 0;
}

uint16 ReadRegWord(uint16 reg) {
  // APU port quirk: 16-bit CMP $2140 must see a CONSISTENT outPorts
  // snapshot. Two separate ReadReg calls would each catch the APU
  // up — between them the SPC could write only the LO byte (port 0)
  // before host has read HI (port 1), so host sees a torn value. Read
  // both ports atomically (single catchup) for the APU-port range.
  if (reg >= 0x2140 && reg <= 0x217F) {
    extern void rtl_accumulate_apu_catchup(void);
    void RtlApuLock(void); void RtlApuUnlock(void);
    void snes_catchupApu(Snes* snes);
    extern Snes *g_snes;
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
    uint8_t lo = g_snes->apu->outPorts[(reg & 0x3)];
    uint8_t hi = g_snes->apu->outPorts[((reg + 1) & 0x3)];
    RtlApuUnlock();
    return (uint16_t)lo | ((uint16_t)hi << 8);
  }
  uint16_t rv = ReadReg(reg);
  rv |= ReadReg(reg + 1) << 8;
  return rv;
}

static void WriteVramWord(Ppu *ppu, uint16 value) {
  uint16_t adr = ppu->vramPointer;
  ppu->vram[adr & 0x7fff] = value;
  // Atomic 16-bit STA $2118 hits both VRAM bytes at this word; record
  // each as a byte event so the differ can compare against the
  // oracle's REGISTER_2118 + REGISTER_2119 byte sequence.
  uint32_t byte_addr = (uint32_t)(adr & 0x7fff) << 1;
  debug_server_on_vram_write(byte_addr,     (uint8_t)(value & 0xff));
  debug_server_on_vram_write(byte_addr + 1, (uint8_t)(value >> 8));
  ppu->vramPointer += ppu->vramIncrement;
}

void WriteRegWord(uint16 reg, uint16 value) {
  if (reg == 0x2118) {
    // VRAM data port: atomic word write
    WriteVramWord(g_ppu, value);
    return;
  }
  // APU port quirk: 16-bit STA $2140 transfers data via $2141 (hi)
  // and the ack-trigger via $2140 (lo). On real hardware both bytes
  // hit the bus together; SMW's SPC IPL upload protocol reads $2141
  // the moment it sees $2140 change. If we write lo first the IPL
  // latches stale $2141. Order hi-then-lo so $2141 is in place
  // before $2140 fires the trigger.
  if (reg >= 0x2140 && reg <= 0x217F) {
    WriteReg(reg + 1, value >> 8);
    WriteReg(reg, (uint8)value);
    return;
  }
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

uint8 *IndirPtr_Slow(LongPtr ptr, uint16 offs) {
  return IndirPtr(ptr, offs);  /* delegates to inline version in header */
}

/* IndirWriteByte is now inline in common_rtl.h */

// Convert the APU-touch cycle delta into APU cycles (ratio ~3.5:1) and
// accumulate into apuCatchupCycles. Caller holds RtlApuLock and is
// responsible for the snes_catchupApu() call. Sets g_apu_last_sync_cycles
// to the current APU-touch estimate so subsequent calls only see
// incremental work. (See g_apu_pace_cycles_estimate's comment for why
// this deliberately ignores non-APU HW touches.)
//
// Public so snes.c's snes_readBBus (the APU read path) can use the same
// pacing -- both reads and writes need to advance APU.
void rtl_accumulate_apu_catchup(void) {
  /* The interp816 bridge advances the SPC per-opcode by guest master cycles
   * (guest-time-anchored, like the ref oracle). Skip the per-touch estimate so
   * an APU-port access inside interpreted code doesn't double-count. */
  if (g_interp_apu_driving) return;
#ifdef SNES_COSIM
  /* Shared APU clock (see common_rtl.h): pure touch-delta pacing, NO wall
   * baseline. The baseline reads the co-sim virtual wall clock, which derives
   * from master_cycles — a per-execution-model quantity (interp charges
   * cyc*8, compiled blocks charge region-weighted static costs), so it would
   * re-introduce the per-side skew this mode exists to remove. Touch credit is
   * deliberately unscaled 256/touch (SNESRECOMP_APU_TOUCH_CYCLES ignored):
   * both sides must use the identical constant. */
  if (cosim_apu_shared_clock()) {
    uint64_t delta = g_apu_pace_cycles_estimate - g_apu_last_sync_cycles;
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
    g_snes->apuCatchupCycles += (double)delta * 2.0 / 7.0;
    g_apu_last_sync_master = g_cpu.master_cycles;
    return;
  }
#endif
#ifdef SNES_COSIM
  /* Co-sim A-vs-B variable-under-test (SNES_COSIM.md task 9): with
   * SNES_COSIM_ACCURATE_APU=1, pace the SPC from the region-weighted master
   * clock at the true SPC:master ratio (exactly like the interp816 ref),
   * instead of the +256/APU-touch synthetic estimate. Two recomp instances
   * that boot IDENTICALLY and differ ONLY in this pacing choice stay
   * bit-identical until synthetic pacing first yields a different APU value —
   * the off-cue's first observable effect, cleanly localized. Cached once. */
  static int s_accurate = -1;
  if (s_accurate < 0) { const char *e = getenv("SNES_COSIM_ACCURATE_APU");
                        s_accurate = (e && e[0] && e[0] != '0') ? 1 : 0; }
  if (s_accurate) {
    static const double kApuPerMaster = (32040.0 * 32.0) / (1364.0 * 262.0 * 60.0);
    uint64_t md = g_cpu.master_cycles - g_apu_last_sync_master;
    g_apu_last_sync_master = g_cpu.master_cycles;
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;   /* keep sync ptr current */
    g_snes->apuCatchupCycles += (double)md * kApuPerMaster;
    return;
  }
#endif
  // NOTE (Axis-5 off-cue experiment, 2026-06-28): pacing the SPC from the
  // recompiler's region-weighted MASTER-clock accumulator (g_cpu.master_cycles,
  // = sum of CPU cycles x code-region speed) was tried here in place of the
  // +256/touch estimate below. Measured A/B vs the bsnes oracle REGRESSED it:
  // SMW drift -4013 -> -5900 ppm, onset match 53% -> 29%. Root: master_cycles
  // counts every recompiled block executed per wall-frame, but the recomp runs
  // frames host-driven (not by counting 357368 master clocks/frame), so its
  // per-frame execution-cycle total over-counts real elapsed time and the SPC
  // over-advances (music runs fast). The accumulator stays emitted as inert
  // Axis-5 infra (companion to cpu->cycles); a correct off-cue cure must pace by
  // WALL time / consumer rate, not accumulated execution cycles -- see
  // SNES_ACCURACY_BURNDOWN.md. Reverted to the known-good touch estimate:
  uint64_t delta = g_apu_pace_cycles_estimate - g_apu_last_sync_cycles;
  g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
  // 2/7 is about 1/3.5 (main MHz / APU MHz). Floor of zero is fine -- short deltas
  // (back-to-back APU touches with no block hooks between them) just don't
  // advance APU on this pass; cycles accumulate for the next touch.
  /* DIAGNOSTIC OVERRIDE (audio hunt, env-gated): rescale the per-touch credit.
   * The estimate charges 256 master cycles per HW touch, but a tight APU poll
   * loop's real period is ~15-25 cycles — an over-credit of >10x that balloons
   * SPC time during port-heavy phases (boot/stage-load uploads measured 17-45x
   * realtime, 220s of APU audio discarded at the output ring). Set
   * SNESRECOMP_APU_TOUCH_CYCLES=20 to charge ~20 master cycles per touch. */
  {
    static double s_touch_scale = -1.0;
    if (s_touch_scale < 0.0) {
      const char *e = getenv("SNESRECOMP_APU_TOUCH_CYCLES");
      s_touch_scale = (e && e[0]) ? (atof(e) / 256.0) : 1.0;
      if (s_touch_scale <= 0.0) s_touch_scale = 1.0;
    }
    delta = (uint64_t)((double)delta * s_touch_scale);
  }
  g_snes->apuCatchupCycles += (double)delta * 2.0 / 7.0;
  // Keep the master sync pointer current so the (now inert) accumulator's delta
  // never balloons if a future change re-enables master-clock pacing.
  g_apu_last_sync_master = g_cpu.master_cycles;

  // Real-time baseline when nothing is draining the DSP output ring
  // (EnableAudio=0, headless runs, the pre-callback boot window, a
  // stalled device). With audio on, the audio thread's RtlRenderAudio
  // top-up advances the SPC at the device's real consumption rate; with
  // no consumer that baseline vanishes and the SPC would advance only on
  // the (possibly rare) APU touches. Inject wall-clock time at the SPC's
  // real rate (~1.024 MHz) so engine state keeps tracking real time and
  // handshakes can never freeze. ADDITIVE to the touch credit, never a
  // limit — handshake over-clock must stay possible (see above).
  // Consumer presence is inferred from sampleRead movement, so this is
  // automatic per game and per moment, no config.
  {
    static uint32_t last_sample_read;
    static uint64_t consume_seen_ms, wall_last_ms;
    Dsp *dsp = g_snes->apu->dsp;
    /* audio_trace_wall_ms() is a deterministic virtual clock under SNES_COSIM
     * (see audio_trace.c) — routes this baseline through the same source as the
     * port-write scheduler, so the whole APU-pacing path is deterministic. */
    uint64_t now_ms = audio_trace_wall_ms();
    uint32_t rd = dsp->sampleRead;
    if (rd != last_sample_read) {
      last_sample_read = rd;
      consume_seen_ms = now_ms;
    }
    int consumer_active = consume_seen_ms != 0 &&
                          now_ms - consume_seen_ms < 250;
    uint32_t baseline = 0;
    if (!consumer_active && wall_last_ms != 0) {
      uint64_t elapsed = now_ms - wall_last_ms;
      if (elapsed > 32) elapsed = 32;  /* burst cap: ~32 ms of SPC time */
      baseline = (uint32_t)(elapsed * 1024);  /* SPC ~1.024 MHz */
      g_snes->apuCatchupCycles += (double)baseline;
    }
    wall_last_ms = now_ms;
    audio_trace_on_pace(consumer_active, baseline);
  }
}

void RtlApuWrite(uint16 adr, uint8 val) {
  assert(adr >= APUI00 && adr <= APUI03);
#ifdef SNES_COSIM
  /* Shared APU clock (common_rtl.h): do NOT convert pending touch credit on a
   * port WRITE — schedule the byte at the current produced clock and leave the
   * credit for the next port READ. Rationale: the interp tier executes a guest
   * word store to $2140/$2141 as two byte writes; converting credit between
   * them runs the SPC ~73 cycles with the kick byte applied but the data byte
   * not yet — the IPL latches stale data and the upload handshake wedges
   * (measured: LLE-shared stuck at outPorts=AABB to frame 360+). On hardware
   * the two bytes land ~2 SPC cycles apart — effectively atomic. Deferring
   * write-side conversion makes byte pairs land at the SAME produced tick, in
   * program order, for BOTH the interp (lo,hi) and compiled (hi,lo) models —
   * which also makes word-write APU evolution identical across tiers. */
  if (cosim_apu_shared_clock()) {
    RtlApuLock();
    audio_trace_on_cpu_port_write((uint8_t)(adr & 0x3), val);
    uint64_t produced_now, consumed_now;
    audio_trace_sample_clocks(&produced_now, &consumed_now);
    apu_schedulePortWrite(g_snes->apu, (uint8_t)(adr & 0x3), val, produced_now);
    RtlApuUnlock();
    return;
  }
#endif
  // Catch the APU up to the current cycle, then SCHEDULE the port write
  // in APU-sample time rather than mutating inPorts at wall time.
  //
  // Rationale (SMW missed-SFX root cause): the audio thread advances
  // the SPC in whole-callback bursts, so a wall-time port mutation gives
  // the value a lifetime of however many samples happen to be produced
  // before the next mutation — measured ~9 samples (vs the 64 an engine
  // poll needs) whenever the 60.0988 Hz NMI beats across the 60.00 Hz
  // callback phase. Anchoring each write one callback quantum past the
  // CONSUMED clock keeps successive frame writes a full frame apart in
  // the SPC's own execution time, so the engine always polls every
  // value, exactly as on hardware. Steady-state added latency is ~zero:
  // consumed + quantum ~= produced, i.e. the next burst applies it.
  // Serialise with the audio thread via RtlApuLock -- it holds the same
  // lock while cycling the APU in RtlRenderAudio.
  RtlApuLock();
  rtl_accumulate_apu_catchup();
  snes_catchupApu(g_snes);
  audio_trace_on_cpu_port_write((uint8_t)(adr & 0x3), val);
  {
    /* Write clock: each target advances from the PREVIOUS write's target
     * by the real wall-time gap between the two writes, converted to
     * samples. This preserves hardware-faithful inter-write spacing in
     * the SPC's execution timeline — frame-spaced NMI writes stay ~534
     * samples apart, same-frame double writes keep their ms-scale gap —
     * independent of where the audio thread's burst boundaries fall.
     *
     * (First attempt anchored targets at consumed+quantum; that fails
     * because produced runs AHEAD of consumed by the output-ring fill,
     * so every target was in the past and the floor collapsed
     * consecutive writes onto the same sample — measured as +0-sample
     * command lifetimes, i.e. the original race in a new costume.)
     *
     * Floor at produced: a target in the APU's past applies on the next
     * executed sample. Ceiling at produced + 3 callback quanta bounds
     * worst-case latency and sheds the slow forward drift from the
     * NMI(60.0988 Hz)/callback(60.00 Hz) rate mismatch. Both caps scale
     * with the observed burst granularity (audio_samples in config.ini
     * is user-tunable): a ceiling smaller than the real burst would pin
     * late-window writes to the same target and re-collapse spacing. */
    static uint64_t s_port_clock;     /* previous write's target */
    static uint64_t s_port_clock_ns;  /* wall_ns of previous write */
    /* Per-port history for the minimum-dwell floor below. Statics, like
     * s_port_clock: not reset across RtlReset/upload, which is benign —
     * after a reset `produced` has advanced far past any stale target, so
     * the floor (stale_target + dwell) is already in the past and never
     * engages spuriously. */
    static uint64_t s_port_last_target[4];
    static uint8_t  s_port_last_val[4];
    static uint8_t  s_port_last_valid[4];
    /* Hardware visibility is the correctness default: a CPU port write lands
     * at the APU's current execution point. Delaying it according to host wall
     * time is not an LLE property and breaks real write -> echo -> poll
     * protocols (MMX's runtime SPC upload used to spend >5 seconds in one
     * faithfully recompiled polling loop). Keep the deferred scheduler below
     * only as an explicit legacy/audio experiment selected with
     * SNESRECOMP_APU_IMMEDIATE_PORTS=0; it must not be a per-game or per-region
     * compile-time correctness hint. */
    static int s_immediate = -1;
    if (s_immediate < 0) {
      const char *e = getenv("SNESRECOMP_APU_IMMEDIATE_PORTS");
      s_immediate = (e && e[0]) ? (e[0] != '0') : 1;
#ifdef SNES_COSIM
      /* Shared APU clock implies immediate ports: the deferred scheduler
       * anchors targets to the co-sim virtual wall clock, which derives from
       * master_cycles — a per-execution-model clock that would re-skew the
       * A/B pair this mode aligns. */
      if (cosim_apu_shared_clock()) s_immediate = 1;
#endif
    }
    if (s_immediate) {
      uint64_t produced_now, consumed_now;
      audio_trace_sample_clocks(&produced_now, &consumed_now);
      apu_schedulePortWrite(g_snes->apu, (uint8_t)(adr & 0x3), val, produced_now);
      RtlApuUnlock();
      return;
    }
    uint64_t quantum = audio_trace_consume_quantum();
    uint64_t now_ns = audio_trace_wall_ns();
    uint64_t produced, consumed;
    audio_trace_sample_clocks(&produced, &consumed);
    uint64_t delta = 0;
    if (s_port_clock_ns != 0)
      delta = (now_ns - s_port_clock_ns) * 32040u / 1000000000u;
    if (delta > 4u * quantum) delta = 4u * quantum;
    uint64_t target = s_port_clock + delta;
    if (target < produced) target = produced;
    if (target > produced + 3u * quantum) target = produced + 3u * quantum;

    /* Minimum per-port dwell — the turbo audio-dropout fix. A level
     * transition fires several DISTINCT values at the same APU port
     * (fade, silence, the new song; or a one-shot command then the NMI's
     * next-frame 0-clear) within a few frames. The wall-clock spacing
     * computed above reproduces hardware timing faithfully at 1x, but
     * turbo runs the game thread uncapped while the SPC still advances at
     * 1x, compressing that spacing below the engine's ~64-sample poll
     * period — so an earlier value is overwritten in inPorts before the
     * engine ever reads it and the command is silently lost (music/SFX
     * drop out; because a surviving fade can zero global output, they do
     * not come back until the next track change, i.e. never within a
     * level). Floor a DISTINCT value's target so the previous distinct
     * value on that port holds the bus for at least APU_PORT_MIN_DWELL
     * produced-samples — one guaranteed engine poll. The drain runs once
     * per produced sample (apu_cycle), so this target spacing becomes
     * apply spacing directly. Bounded by produced + 8*quantum so a
     * pathological sustained burst degrades to today's bounded latency
     * rather than unbounding it. Identical repeats (e.g. repeated
     * 0-clears) need no spacing. No effect at 1x: frame-spaced writes are
     * already ~534 samples apart, far above the floor. */
    {
      int p = (int)(adr & 0x3);
      if (s_port_last_valid[p] && val != s_port_last_val[p]) {
        uint64_t floor = s_port_last_target[p] + APU_PORT_MIN_DWELL;
        uint64_t ceil  = produced + 8u * quantum;
        if (target < floor) target = floor < ceil ? floor : ceil;
      }
      s_port_last_target[p] = target;
      s_port_last_val[p]    = val;
      s_port_last_valid[p]  = 1;
    }

    s_port_clock = target;
    s_port_clock_ns = now_ns;
    apu_schedulePortWrite(g_snes->apu, (uint8_t)(adr & 0x3), val, target);
  }
  RtlApuUnlock();
}

static bool RtlUploadSpcImageFromDpInternal(CpuState *cpu, bool update_cpu_result) {
  uint16_t dp = cpu->D;
  uint16_t data_lo = (uint16_t)g_ram[(dp + 0) & 0xffff]
                   | ((uint16_t)g_ram[(dp + 1) & 0xffff] << 8);
  uint8_t data_bank = g_ram[(dp + 2) & 0xffff];
  const uint8_t *p = RomPtr(((uint32_t)data_bank << 16) | data_lo);
  uint16_t final_pc = 0;
  int block_count = 0;

  RtlApuLock();
  for (;;) {
    uint16_t n = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    uint16_t target = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
    p += 4;
    if (n == 0) {
      final_pc = target;
      break;
    }
    for (uint16_t i = 0; i < n; i++)
      g_snes->apu->ram[(uint16_t)(target + i)] = p[i];
    p += n;
    if (++block_count > 512) {
      RtlApuUnlock();
      fprintf(stderr, "[apu] bad SPC upload stream at %02X:%04X\n",
              data_bank, data_lo);
      return false;
    }
  }

  /* First-upload vs subsequent-upload semantics differ. The very first
   * upload from CPU after reset goes through the SNES SPC IPL bootROM,
   * which ends with `JMP [$0000+X]` — i.e. the IPL jumps to the entry
   * address provided in the terminator's target field. After that
   * first upload, the IPL is mapped out (romReadable=false) and the
   * loaded SPC engine handles all subsequent CPU upload requests via
   * its own routine (SMW's StandardTransfer at SPC $12F2). That
   * routine just RETs at the end — it does NOT jump to any entry
   * point. The terminator's target field is benign on subsequent
   * uploads.
   *
   * If we unconditionally re-jumped SPC PC to the terminator entry,
   * every music-bank upload would restart APU_Start, zero-clearing
   * the engine's music state ($00-$E7 + ARAM_0386-9) and the
   * just-uploaded music data would never start playing. SFX would
   * still work since they're triggered by inPort writes processed
   * after the restart's re-init, but song state would never persist.
   *
   * Detect "first upload" via apu->romReadable: it's reset to true by
   * apu_reset() and only flipped false here, so on the IPL-phase
   * upload it's still true. */
  bool ipl_phase = g_snes->apu->romReadable;
  /* The upload supersedes any not-yet-applied scheduled port writes;
   * a stale pre-upload command landing on the freshly cleared ports
   * would replay into the re-initialised engine. */
  apu_clearPortQueue(g_snes->apu);
  memset(g_snes->apu->inPorts, 0, sizeof(g_snes->apu->inPorts));
  memset(g_snes->apu->outPorts, 0, sizeof(g_snes->apu->outPorts));
  if (ipl_phase) {
    g_snes->apu->romReadable = false;
    g_snes->apuCatchupCycles = 0;
    g_snes->apu->cpuCyclesLeft = 0;
    if (final_pc != 0) {
      g_snes->apu->spc->a = 0;
      g_snes->apu->spc->x = 0;
      g_snes->apu->spc->y = 0;
      if (g_snes->apu->spc->sp == 0)
        g_snes->apu->spc->sp = 0xef;
      g_snes->apu->spc->pc = final_pc;
    }
  }
  g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
  // Resync the master pointer too: an IPL-phase upload zeroes apuCatchupCycles,
  // so the next catch-up must start its delta from here, not replay the master
  // cycles burned during the upload spin.
  g_apu_last_sync_master = g_cpu.master_cycles;
  RtlApuUnlock();

  if (update_cpu_result) {
    cpu->A = (uint16_t)(cpu->A & 0xff00);
    cpu->X = 0;
    cpu->Y = 0;
    cpu->_flag_Z = 1;
    cpu->_flag_N = 0;
    cpu->P = (uint8_t)((cpu->P & ~0x82) | 0x02);
  }
  return true;
}

bool RtlUploadSpcImageFromDp(CpuState *cpu) {
  return RtlUploadSpcImageFromDpInternal(cpu, false);
}

bool RtlHandleSpcUpload(CpuState *cpu) {
  return RtlUploadSpcImageFromDpInternal(cpu, true);
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
  assert(channels == 2);
  /* Cycle the APU in small batches under the lock, releasing between
   * each so the CPU thread (RtlApuWrite / snes_readBBus) can make
   * progress. Earlier code held RtlApuLock for the entire 17 000-cycle
   * loop, which took ~4 ms host time per audio callback. With audio
   * callbacks at ~60 Hz that pinned the CPU thread out of the lock for
   * ~27 % of wall time, and the SMW IPL upload (which touches APU
   * ports thousands of times) ran an order of magnitude slower than
   * the watchdog allowed.
   *
   * 256 SPC cycles per batch is about 64 us host work per acquire, short
   * enough that the CPU thread's RtlApuLock call almost never has to
   * wait through a full audio batch. apu_cycle is single-threaded
   * regardless -- the lock just serialises access to inPorts/outPorts
   * shared with the CPU thread. */
  // Ensure at least one block (534 native samples) is available in the
  // ring, then consume it. The audio thread only produces the shortfall
  // the CPU-thread catch-up (snes_catchupApu) hasn't already supplied, so
  // it self-balances: total SPC advance stays at the consumption rate and
  // bursty catch-up production is buffered, not dropped.
  #define DSP_AVAIL(d) ((uint32_t)((d)->sampleWrite - (d)->sampleRead))
  while (DSP_AVAIL(g_snes->apu->dsp) < 534) {
    RtlApuLock();
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_AUDIO);
    int batch = 256;
    while (batch-- > 0 && DSP_AVAIL(g_snes->apu->dsp) < 534)
      apu_cycle(g_snes->apu);
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
    RtlApuUnlock();
  }
  #undef DSP_AVAIL
  RtlApuLock();
  dsp_getSamples(g_snes->apu->dsp, audio_buffer, samples);
  /* Mix MSU-1 streaming audio on top of the S-DSP block. Inert (no-op)
   * unless a pack is armed and a track is playing. Runs under the APU
   * lock we already hold, which serialises it against MSU register
   * writes on the CPU thread (msu1_read/msu1_write take the same lock). */
  msu1_mix(audio_buffer, samples);
  RtlApuUnlock();
}

/* The battery-backed SRAM lives at a fixed, game-agnostic path next to the exe
 * (each game has its own directory, so there is no collision). Older builds named
 * it after the game's internal title — which happened to be "smw" for every game
 * (a copy-paste leftover), so Mega Man X / Zelda also wrote saves/smw.srm.
 * RtlMigrateLegacySram copies any such legacy save forward the first time the new
 * generic path is used, so existing players keep their progress. */
#define RTL_SRAM_FILE     "saves/save.srm"
#define RTL_SRAM_BAK_FILE "saves/save.srm.bak"

void RtlMigrateLegacySram(const char *legacy_title) {
  if (!legacy_title || !*legacy_title) return;
  FILE *cur = fopen(RTL_SRAM_FILE, "rb");
  if (cur) { fclose(cur); return; }   /* already on the generic name */
  char legacy[64];
  snprintf(legacy, sizeof(legacy), "saves/%s.srm", legacy_title);
  if (strcmp(legacy, RTL_SRAM_FILE) == 0) return;  /* legacy name IS the generic one */
  FILE *in = fopen(legacy, "rb");
  if (!in) return;                    /* no legacy save to carry forward */
  FILE *out = fopen(RTL_SRAM_FILE, "wb");
  if (!out) { fclose(in); return; }   /* e.g. saves/ not writable */
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  fprintf(stderr, "[saves] migrated legacy %s -> %s\n", legacy, RTL_SRAM_FILE);
}

void RtlReadSram(void) {
  RtlMigrateLegacySram(g_rtl_game_info->title);
  FILE *f = fopen(RTL_SRAM_FILE, "rb");
  if (f) {
    if (fread(g_sram, 1, g_sram_size, f) != g_sram_size)
      fprintf(stderr, "Error reading %s\n", RTL_SRAM_FILE);
    fclose(f);
  }
}

void RtlWriteSram(void) {
  rename(RTL_SRAM_FILE, RTL_SRAM_BAK_FILE);
  FILE *f = fopen(RTL_SRAM_FILE, "wb");
  if (f) {
    fwrite(g_sram, 1, g_sram_size, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write %s\n", RTL_SRAM_FILE);
  }
}

static const uint8 *SimpleHdma_GetPtr(uint32 p) {
  uint8 bank = (uint8)(p >> 16);
  uint16 addr = (uint16)(p & 0xffff);
  if (bank == 0x7E) return g_ram + addr;
  if (bank == 0x7F) return g_ram + 0x10000 + addr;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && addr < 0x2000)
    return g_ram + addr;
  return RomPtr(p);
}

/* RomPtr() mirrors the initial SNES address into the loaded cartridge, but
 * SimpleHdma_DoLine advances the returned host pointer directly. A malformed
 * or unterminated table can therefore step one byte past WRAM or the ROM
 * allocation. Validate every host-side read and treat the end of either
 * backing store like an HDMA table terminator. Use integer address arithmetic
 * here: relational comparisons between pointers to different C objects are
 * not portable. */
static bool SimpleHdma_PtrRangeValid(const uint8 *p, size_t length) {
  uintptr_t addr = (uintptr_t)p;
  uintptr_t ram_base = (uintptr_t)g_ram;
  if (addr >= ram_base) {
    size_t offset = (size_t)(addr - ram_base);
    if (offset <= sizeof(g_ram) && length <= sizeof(g_ram) - offset)
      return true;
  }

  uint32 rom_size = g_snes && g_snes->cart
                  ? (uint32)g_snes->cart->romSize : 0;
  uintptr_t rom_base = (uintptr_t)g_rom;
  if (g_rom && rom_size != 0 && addr >= rom_base) {
    size_t offset = (size_t)(addr - rom_base);
    if (offset <= rom_size && length <= (size_t)rom_size - offset)
      return true;
  }
  return false;
}

void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc) {
  if (!dc->hdmaActive) {
    c->table = 0;
    return;
  }
  c->table = SimpleHdma_GetPtr(dc->aAdr | dc->aBank << 16);
  c->rep_count = 0;
  c->mode = dc->mode | dc->indirect << 6;
  c->ppu_addr = dc->bAdr;
  c->indir_bank = dc->indBank;
}

void SimpleHdma_DoLine(SimpleHdma *c) {
  static const uint8 bAdrOffsets[8][4] = {
    {0, 0, 0, 0},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1},
    {0, 1, 2, 3},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1}
  };
  static const uint8 transferLength[8] = {
    1, 2, 2, 4, 4, 4, 2, 4
  };

  if (c->table == NULL)
    return;
  bool do_transfer = false;
  if ((c->rep_count & 0x7f) == 0) {
    if (!SimpleHdma_PtrRangeValid(c->table, 1)) {
      c->table = NULL;
      return;
    }
    c->rep_count = *c->table++;
    if (c->rep_count == 0) {
      c->table = NULL;
      return;
    }
    if(c->mode & 0x40) {
      if (!SimpleHdma_PtrRangeValid(c->table, 2)) {
        c->table = NULL;
        return;
      }
      c->indir_ptr = SimpleHdma_GetPtr(c->indir_bank << 16 | c->table[0] | c->table[1] * 256);
      c->table += 2;
    }
    do_transfer = true;
  }
  if(do_transfer || c->rep_count & 0x80) {
    for(int j = 0, j_end = transferLength[c->mode & 7]; j < j_end; j++) {
      const uint8 *src = c->mode & 0x40 ? c->indir_ptr : c->table;
      if (!SimpleHdma_PtrRangeValid(src, 1)) {
        c->table = NULL;
        break;
      }
      uint8 v = *src;
      if (c->mode & 0x40)
        c->indir_ptr++;
      else
        c->table++;
      /* ppu_write takes the B-bus offset ($00-$3F), not a $21xx CPU addr. */
      uint8 reg = (uint8)(c->ppu_addr + bAdrOffsets[c->mode & 7][j]);
      ppu_write(g_ppu, reg, v);
      debug_server_on_reg_write((uint16)(0x2100u + reg), v);
    }
  }
  c->rep_count--;
}
