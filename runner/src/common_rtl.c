#include "common_rtl.h"
#include "common_cpu_infra.h"
#include <setjmp.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include "recomp_hw.h"
#include "framedump.h"
#include "util.h"
#include "config.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/msu1.h"
#include "snes/ws_shadow.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "audio_trace.h"
#include "ppu_dma_trace.h"
#include "host_report.h"
#include "cosim.h"
#if defined(SNESRECOMP_NET)
#include "snes_netplay.h"
#endif

uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;
uint8 g_snesrecomp_last_hdmaen;

/* Netplay suppresses the pre-frame wall-clock fallback below. Once frames
 * begin, every runner uses the same guest-frame/APU coupling, and the audio
 * callback only consumes samples without advancing emulation. */
static int rtl_netplay_locks_audio(void) {
#if defined(SNESRECOMP_NET)
  return snes_netplay_active();
#else
  return 0;
#endif
}

void RtlNetplayAudioReset(void) {
  /* Kept as a stable facade hook for recomp-net hosts. The current runner's
   * audio callback is already consumer-only, so there is no separate
   * wall-clock netplay accumulator to reset. */
}

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
/* Production's frame loop is the stable guest-time ruler: one frame is
 * 357368 SNES master cycles and 17088 SPC cycles in this runtime's 60 Hz
 * model. g_cpu.master_cycles supplies only the within-frame position because
 * its total per frame varies with recompilation coverage. */
#define RTL_MASTER_CYCLES_PER_FRAME 357368ull
#define RTL_APU_CYCLES_PER_FRAME     17088ull
static uint64_t g_apu_frame_start_master;
static bool g_apu_frame_time_valid;

/* Fast-forward advances the real SPC/DSP state faster than the host device can
 * play it. On the transition back to realtime, buffered PCM represents stale
 * guest time and must not become permanent A/V latency. The short ramp joins
 * the last delivered sample to the first current sample without a hard edge. */
#define RTL_AUDIO_RECOVERY_RAMP 128u
static bool g_audio_fast_forward;
static uint32_t g_audio_recovery_frames;
static uint32_t g_audio_recovery_remaining;
static int16 g_audio_recovery_anchor_l;
static int16 g_audio_recovery_anchor_r;
static int16 g_audio_last_output_l;
static int16 g_audio_last_output_r;
static void rtl_sync_apu_frame_boundary(void);

static uint64_t rtl_apu_guest_cycle(void) {
  uint64_t within = g_cpu.master_cycles - g_apu_frame_start_master;
  if (within >= RTL_MASTER_CYCLES_PER_FRAME)
    within = RTL_MASTER_CYCLES_PER_FRAME - 1;
  return (uint64_t)snes_frame_counter * RTL_APU_CYCLES_PER_FRAME +
         within * RTL_APU_CYCLES_PER_FRAME /
             RTL_MASTER_CYCLES_PER_FRAME;
}
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
  g_apu_frame_time_valid = false;
  g_apu_frame_start_master = g_cpu.master_cycles;
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
  g_audio_fast_forward = false;
  g_audio_recovery_frames = 0;
  g_audio_recovery_remaining = 0;
  g_audio_recovery_anchor_l = 0;
  g_audio_recovery_anchor_r = 0;
  g_audio_last_output_l = 0;
  g_audio_last_output_r = 0;
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

  /* Establish the guest timestamp origin before any frame code can touch an
   * APU port. Host turbo changes how quickly frames arrive, not their guest
   * duration. */
  g_apu_frame_start_master = g_cpu.master_cycles;
  g_apu_frame_time_valid = true;
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
    if (s_audio && !rtl_netplay_locks_audio()) {
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
  /* Every runner client gets the same guest-frame/APU coupling. Presentation
   * code may opt into fast-forward PCM recovery separately, but cannot omit
   * the emulation clock. */
  rtl_sync_apu_frame_boundary();

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

bool RtlSaveSnapshot(const char *filename) {
  FILE *f = fopen(filename, "wb");
  if (!f) {
    printf("Failed fopen for save: %s\n", filename);
    return false;
  }
  uint32 hdr[2] = { RTL_SAV_MAGIC, RTL_SAV_VERSION };
  bool header_ok = fwrite(hdr, sizeof(hdr), 1, f) == 1;
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, true, !header_ok };
  if (header_ok)
    snes_saveload(g_snes, &fs.base);
  /* v5: game-specific chunk (task-slot resume contexts etc.). Streamed
   * through the same FileSli so the format stays one linear blob. */
  if (g_rtl_game_info && g_rtl_game_info->state_save_extra)
    g_rtl_game_info->state_save_extra(&fs.base);
  RtlApuUnlock();
  if (fs.error) printf("Save write error: %s\n", filename);
  bool close_ok = fclose(f) == 0;
  return !fs.error && close_ok;
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
  /* v5+: an optional game-specific chunk follows the guest blob. Only call
   * the loader when trailing bytes remain so older v5 snapshots created by
   * games without an extra chunk remain readable. */
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
  g_snes->beamMasterLast = g_cpu.master_cycles;
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
  g_snes->beamMasterLast = g_cpu.master_cycles;
  if (g_rtl_game_info && g_rtl_game_info->on_state_loaded)
    g_rtl_game_info->on_state_loaded(hdr[1]);
  return true;
}

void RtlSaveLoad(int cmd, int slot) {
  char name[128];
  RtlEnsureSaveDir();
  RtlSaveSlotPath(slot, name, sizeof(name));
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
  // both ports atomically (single guest-time sync) for the APU-port range.
  if (reg >= 0x2140 && reg <= 0x217F) {
    void RtlApuLock(void); void RtlApuUnlock(void);
    extern Snes *g_snes;
    RtlApuLock();
    rtl_sync_apu_to_cpu_locked();
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
  WsShadowOnVramWrite((uint16_t)(adr & 0x7fff), value);
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
  //
  // Netplay skips this: wall time differs per peer and desyncs LLE. Once the
  // frame loop starts, rtl_sync_apu_frame_boundary() is authoritative.
  {
    static uint32_t last_sample_read;
    static uint64_t consume_seen_ms, wall_last_ms;
    Dsp *dsp = g_snes->apu->dsp;
    /* audio_trace_wall_ms() is a deterministic virtual clock under SNES_COSIM
     * (see audio_trace.c) — routes this baseline through the same source as the
     * port-write scheduler, so the whole APU-pacing path is deterministic. */
    uint64_t now_ms = audio_trace_wall_ms();
    if (rtl_netplay_locks_audio()) {
      wall_last_ms = now_ms;
      audio_trace_on_pace(1, 0);
      return;
    }
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
  uint8_t port = (uint8_t)(adr & 3);

#ifdef SNES_COSIM
  /* The shared-clock co-sim advances the SPC synchronously and compares two
   * execution models instruction-for-instruction. Keep its bus mutation at
   * the already-aligned current clock. */
  if (cosim_apu_shared_clock()) {
    RtlApuLock();
    audio_trace_on_cpu_port_write(port, val);
    apu_writePortNow(g_snes->apu, port, val);
    RtlApuUnlock();
    return;
  }
#endif

  RtlApuLock();
  if (!g_apu_frame_time_valid) {
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
  }
  audio_trace_on_cpu_port_write(port, val);

  if (!g_apu_frame_time_valid) {
    /* Reset/IPL handshakes actively advance the SPC through their poll loops;
     * there is no host frame timeline yet, so current-cycle visibility is the
     * faithful model. */
    apu_writePortNow(g_snes->apu, port, val);
  } else {
    uint64_t guest_cycle = rtl_apu_guest_cycle();
    while (!apu_schedulePortWrite(g_snes->apu, port, val, guest_cycle))
      apu_cycle(g_snes->apu);
  }
  RtlApuUnlock();
}

void rtl_sync_apu_to_cpu_locked(void) {
  if (!g_apu_frame_time_valid) {
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
    return;
  }
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  uint64_t before = g_snes->apu->portClock;
  bool synced = apu_runToGuestCycle(g_snes->apu, rtl_apu_guest_cycle(),
                                    1u << 20);
  audio_trace_on_guest_sync(0, g_snes->apu->portClock - before);
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
  if (!synced)
    fprintf(stderr, "[apu] CPU-port guest-clock sync timed out\n");
}

static bool RtlUploadSpcImageFromDpInternal(CpuState *cpu,
                                            bool update_cpu_result,
                                            bool live_transfer) {
  uint16_t dp = cpu->D;
  uint16_t data_lo = (uint16_t)g_ram[(dp + 0) & 0xffff]
                   | ((uint16_t)g_ram[(dp + 1) & 0xffff] << 8);
  uint8_t data_bank = g_ram[(dp + 2) & 0xffff];
  const uint8_t *p = RomPtr(((uint32_t)data_bank << 16) | data_lo);
  uint16_t final_pc = 0;
  int block_count = 0;

  RtlApuLock();
  bool ipl_phase = g_snes->apu->romReadable;
  /* The HLE routine takes ownership only after all earlier CPU bus events
   * have reached the live SPC. For a running driver, honor its ready
   * handshake before replacing the byte-by-byte payload copy. */
  if (!apu_runUntilPortQueueEmpty(g_snes->apu, 1u << 22)) {
    RtlApuUnlock();
    fprintf(stderr, "[apu] queued port events did not reach upload boundary\n");
    return false;
  }
  uint8_t transfer_request = g_snes->apu->inPorts[1];
  if (live_transfer && !ipl_phase &&
      !apu_waitForTransferReady(g_snes->apu, 1, transfer_request, 1u << 20)) {
    fprintf(stderr,
            "[apu] SPC transfer-ready timeout pc=%04x stopped=%d "
            "in=%02x%02x out=%02x%02x edge=%02x%02x buf=%02x%02x\n",
            g_snes->apu->spc->pc, g_snes->apu->spc->stopped,
            g_snes->apu->inPorts[0], g_snes->apu->inPorts[1],
            g_snes->apu->outPorts[0], g_snes->apu->outPorts[1],
            g_snes->apu->ram[0], g_snes->apu->ram[1],
            g_snes->apu->ram[4], g_snes->apu->ram[5]);
    RtlApuUnlock();
    return false;
  }
  apu_clearPortQueue(g_snes->apu);
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
  if (ipl_phase) {
    /* The boot upload body was replaced wholesale, so this is the logical
     * consumption of the request that selected it. */
    audio_trace_on_spc_port_read(1, transfer_request);
    memset(g_snes->apu->inPorts, 0, 4);
    memset(g_snes->apu->outPorts, 0, sizeof(g_snes->apu->outPorts));
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
  } else if (live_transfer) {
    if (!apu_finishHleTransfer(g_snes->apu, final_pc, 1u << 20)) {
      RtlApuUnlock();
      fprintf(stderr, "[apu] SPC transfer terminator timed out\n");
      return false;
    }
  } else {
    /* Legacy HLE users have not declared a live driver protocol. Preserve
     * their prior behavior instead of guessing that they speak SMW's
     * AA/BB/CC transfer handshake. */
    memset(g_snes->apu->inPorts, 0, 4);
    memset(g_snes->apu->outPorts, 0, sizeof(g_snes->apu->outPorts));
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
  return RtlUploadSpcImageFromDpInternal(cpu, false, false);
}

bool RtlUploadSpcImageFromDpLive(CpuState *cpu) {
  return RtlUploadSpcImageFromDpInternal(cpu, false, true);
}

bool RtlHandleSpcUpload(CpuState *cpu) {
  return RtlUploadSpcImageFromDpInternal(cpu, true, false);
}

static void rtl_sync_apu_frame_boundary(void) {
  /* The game frame is the authoritative guest-time clock. The audio callback
   * may fill a host scheduling shortfall, but CPU->APU events must never wait
   * behind it: advance the real SPC through every event due by this completed
   * frame at normal speed and turbo alike. */
  RtlApuLock();
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  uint64_t before = g_snes->apu->portClock;
  /* RtlRunFrame has already incremented snes_frame_counter. This is the exact
   * boundary after the completed frame; adding its stale within-frame master
   * offset here would count the frame body twice. */
  uint64_t boundary = (uint64_t)snes_frame_counter *
                      RTL_APU_CYCLES_PER_FRAME;
  bool synced = apu_runToGuestCycle(g_snes->apu, boundary,
                                    1u << 20);
  audio_trace_on_guest_sync(1, g_snes->apu->portClock - before);
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
  if (!synced)
    fprintf(stderr, "[apu] frame-boundary guest-clock sync timed out\n");
  RtlApuUnlock();
}

void RtlAudioSetFastForward(bool active) {
  if (!active && !g_audio_fast_forward && g_audio_recovery_frames == 0)
    return;
  RtlApuLock();
  if (active && !g_audio_fast_forward) {
    /* A new fast-forward interval supersedes the prior continuity ramp. */
    g_audio_recovery_frames = 0;
    g_audio_recovery_remaining = 0;
  } else if (!active && g_audio_fast_forward) {
    g_audio_recovery_frames = 30;
    g_audio_recovery_anchor_l = g_audio_last_output_l;
    g_audio_recovery_anchor_r = g_audio_last_output_r;
    g_audio_recovery_remaining = RTL_AUDIO_RECOVERY_RAMP;
  }
  if (!active && g_audio_recovery_frames != 0) {
    uint32_t available = g_snes->apu->dsp->sampleWrite -
                         g_snes->apu->dsp->sampleRead;
    /* Keep two current blocks: one for the next callback and one scheduling
     * cushion. Repeat at frame boundaries only while post-turbo CPU work is
     * settling, then restore the ordinary FIFO unchanged. */
    uint32_t discarded = dsp_trimSamples(g_snes->apu->dsp, 1068);
    if (discarded != 0) {
      audio_trace_on_fast_forward_discard(discarded,
                                           available - discarded);
      g_audio_recovery_anchor_l = g_audio_last_output_l;
      g_audio_recovery_anchor_r = g_audio_last_output_r;
      g_audio_recovery_remaining = RTL_AUDIO_RECOVERY_RAMP;
      /* The host frame-delay clock may still be catching up after a long
       * turbo interval. Require a full stable window after the most recent
       * trim instead of expiring recovery after a fixed number of fast game
       * frames. This makes release behavior independent of turbo duration. */
      g_audio_recovery_frames = 30;
    } else {
      g_audio_recovery_frames--;
    }
  }
  g_audio_fast_forward = active;
  RtlApuUnlock();
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
  assert(channels == 2);
  /* SPC state is guest-frame driven by RtlAudioSyncFrame. The host callback is
   * a consumer only: allowing it to invent SPC cycles makes its wall-clock
   * schedule a second, competing emulation clock and is what let audio drift
   * behind visuals after turbo. */
  RtlApuLock();
  uint32_t available = g_snes->apu->dsp->sampleWrite -
                       g_snes->apu->dsp->sampleRead;
  if (available >= 534) {
    dsp_getSamples(g_snes->apu->dsp, audio_buffer, samples);
  } else {
    /* Do not advance guest state to hide host scheduling jitter. Preserve a
     * partial block for the next callback and emit one block of silence. */
    memset(audio_buffer, 0, (size_t)samples * 2 * sizeof(*audio_buffer));
    audio_trace_on_output_underflow(available);
  }
  /* Mix MSU-1 streaming audio on top of the S-DSP block. Inert (no-op)
   * unless a pack is armed and a track is playing. Runs under the APU
   * lock we already hold, which serialises it against MSU register
   * writes on the CPU thread (msu1_read/msu1_write take the same lock). */
  msu1_mix(audio_buffer, samples);
  for (int i = 0; i < samples && g_audio_recovery_remaining != 0; i++) {
    uint32_t progressed = RTL_AUDIO_RECOVERY_RAMP -
                          g_audio_recovery_remaining + 1;
    uint32_t old_weight = RTL_AUDIO_RECOVERY_RAMP - progressed;
    audio_buffer[i * 2] = (int16)(((int32_t)g_audio_recovery_anchor_l *
                                   (int32_t)old_weight +
                                   (int32_t)audio_buffer[i * 2] *
                                   (int32_t)progressed) /
                                  (int32_t)RTL_AUDIO_RECOVERY_RAMP);
    audio_buffer[i * 2 + 1] = (int16)(((int32_t)g_audio_recovery_anchor_r *
                                       (int32_t)old_weight +
                                       (int32_t)audio_buffer[i * 2 + 1] *
                                       (int32_t)progressed) /
                                      (int32_t)RTL_AUDIO_RECOVERY_RAMP);
    g_audio_recovery_remaining--;
  }
  if (samples > 0) {
    g_audio_last_output_l = audio_buffer[(samples - 1) * 2];
    g_audio_last_output_r = audio_buffer[(samples - 1) * 2 + 1];
  }
  RtlApuUnlock();
}

/* Battery-backed SRAM + savestate slots live under RtlSaveRoot() (default
 * "saves/"). Netplay guests switch the root to "saves/netplay/" so host sync
 * cannot overwrite personal progress. Older builds named the SRAM after the
 * game's internal title ("smw" for every title); RtlMigrateLegacySram copies
 * that forward the first time the generic name is used under the main root. */
static char s_save_root[96] = "saves";

void RtlSetSaveRoot(const char *root) {
  if (!root || !root[0])
    snprintf(s_save_root, sizeof(s_save_root), "saves");
  else {
    snprintf(s_save_root, sizeof(s_save_root), "%s", root);
    /* Trim trailing slash */
    size_t n = strlen(s_save_root);
    while (n > 1 && (s_save_root[n - 1] == '/' || s_save_root[n - 1] == '\\')) {
      s_save_root[n - 1] = '\0';
      n--;
    }
  }
}

const char *RtlSaveRoot(void) { return s_save_root; }

void RtlEnsureSaveDir(void) {
#ifdef _WIN32
  _mkdir(s_save_root);
  /* Also ensure parent "saves" when root is saves/netplay */
  if (strncmp(s_save_root, "saves/", 6) == 0 || strncmp(s_save_root, "saves\\", 6) == 0)
    _mkdir("saves");
#else
  mkdir(s_save_root, 0755);
  if (strncmp(s_save_root, "saves/", 6) == 0)
    mkdir("saves", 0755);
#endif
}

void RtlSaveSlotPath(int slot, char *buf, size_t buflen) {
  const char *prefix = g_rtl_game_info ? g_rtl_game_info->save_name_prefix : NULL;
  if (prefix)
    snprintf(buf, buflen, "%s/%s%d.sav", s_save_root, prefix, slot);
  else if (g_rtl_game_info && g_rtl_game_info->title)
    snprintf(buf, buflen, "%s/%s_save%d.sav", s_save_root, g_rtl_game_info->title, slot);
  else
    snprintf(buf, buflen, "%s/save%d.sav", s_save_root, slot);
}

void RtlSramFilePath(char *buf, size_t buflen) {
  snprintf(buf, buflen, "%s/save.srm", s_save_root);
}

void RtlMigrateLegacySram(const char *legacy_title) {
  /* Only migrate into the main offline root — never into a netplay sandbox. */
  if (!legacy_title || !*legacy_title) return;
  if (strcmp(s_save_root, "saves") != 0) return;
  char cur_path[128];
  RtlSramFilePath(cur_path, sizeof(cur_path));
  FILE *cur = fopen(cur_path, "rb");
  if (cur) { fclose(cur); return; }   /* already on the generic name */
  char legacy[64];
  snprintf(legacy, sizeof(legacy), "saves/%s.srm", legacy_title);
  if (strcmp(legacy, cur_path) == 0) return;
  FILE *in = fopen(legacy, "rb");
  if (!in) return;
  RtlEnsureSaveDir();
  FILE *out = fopen(cur_path, "wb");
  if (!out) { fclose(in); return; }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  fprintf(stderr, "[saves] migrated legacy %s -> %s\n", legacy, cur_path);
}

void RtlReadSram(void) {
  char path[128];
  RtlMigrateLegacySram(g_rtl_game_info->title);
  RtlSramFilePath(path, sizeof(path));
  FILE *f = fopen(path, "rb");
  if (f) {
    if (fread(g_sram, 1, g_sram_size, f) != g_sram_size)
      fprintf(stderr, "Error reading %s\n", path);
    fclose(f);
  }
}

void RtlWriteSram(void) {
  char path[128], bak[140];
  RtlEnsureSaveDir();
  RtlSramFilePath(path, sizeof(path));
  snprintf(bak, sizeof(bak), "%s.bak", path);
  rename(path, bak);
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite(g_sram, 1, g_sram_size, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write %s\n", path);
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
      /* ppu_write takes the B-bus offset ($00-$3F), not a $21xx CPU address. */
      uint8 reg = (uint8)(c->ppu_addr + bAdrOffsets[c->mode & 7][j]);
      ppu_write(g_ppu, reg, v);
      debug_server_on_reg_write((uint16)(0x2100u + reg), v);
    }
  }
  c->rep_count--;
}
