/*
 * snes9x_bridge.cpp — snes9x-libretro oracle backend.
 *
 * Loads snes9x as a statically-linked oracle emulator alongside the
 * recompiled code, exposing a snes_oracle_backend_t that the generic
 * emu_oracle_cmds.c dispatches through. Only compiled in the Oracle
 * MSBuild configuration.
 *
 * Mirrors the Nestopia oracle pattern at
 * F:/Projects/nesrecomp/runner/src/nestopia_bridge.cpp, adapted to
 * snes9x's libretro API and 65816 register layout.
 */
#include "snes9x_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* snes9x libretro glue API. */
#include "libretro.h"

/* snes9x internals — global Memory / Registers / CPU objects. These
 * headers transitively pull a lot of C++ tangle; keeping includes
 * local to this TU contains the damage. */
#include "snes9x.h"
#include "memmap.h"
#include "65c816.h"
#include "getset.h"

namespace {

/* ---- State ---- */
uint32_t  s_framebuf_xrgb[256 * 240] = {0};
unsigned  s_frame_width  = 256;
unsigned  s_frame_height = 224;
uint16_t  s_joypad[2]    = {0, 0};
bool      s_loaded       = false;
/* Pixel format the core requested via SET_PIXEL_FORMAT (libretro enum:
 * 0=0RGB1555, 1=XRGB8888, 2=RGB565). snes9x-libretro requests RGB565 by
 * default, so we must convert in retro_video_refresh rather than assume
 * 32-bit (assuming 32-bit produced garbled/wrong-colour screenshots). */
unsigned  s_pixel_format = 2; /* default RGB565 (snes9x default) */

/* WRAM snapshot from before the most-recent retro_run(). Enables
 * emu_wram_delta: per-frame write observability without touching
 * snes9x's memory bus. */
uint8_t   s_wram_before[0x20000] = {0};

/* ---- Tier-1-equivalent WRAM write watchpoint ----
 *
 * snes9x calls s9x_write_hook (declared extern in getset.h, defined
 * below) on every write that goes through the memory bus. Our hook
 * checks whether Address falls in any armed watchpoint range and, if
 * so, records (frame, Address, Byte, PC, PB) into a ring. The ring
 * is read back via emu_get_wram_trace.
 *
 * Scoped to WRAM ($00:0000-$00:1FFF mirror of bank 7E, plus explicit
 * bank-7E and bank-7F accesses). Other bus writes are ignored by the
 * filter even when hook is installed.
 */
#define EMU_WATCH_MAX_RANGES 8
// Always-on full-WRAM trace records every snes9x WRAM write from
// process start. SMW writes ~500-1000 WRAM bytes per frame; a 16K
// ring evicts early-boot writes within ~30 frames, hiding the very
// init sequences we want to query backward in history. Mirror
// recomp's WRAM_TRACE_LOG_SIZE = 1M; entry size is 16 bytes so
// resident cost is ~16 MB. Acceptable for the Oracle build (debug-
// only), and lets the ring hold ~thousands of frames before wrap.
#define EMU_WATCH_LOG_SIZE   (1 << 20)

struct emu_watch_range { uint32_t lo, hi; };
struct emu_watch_entry {
    uint32_t frame;
    uint32_t addr;      /* 20-bit WRAM offset */
    uint32_t pc24;      /* 24-bit bank:pc at hook time */
    uint8_t  byte_before;
    uint8_t  byte_after;
    uint16_t bank_source;  /* full 24-bit address mod (0x7e0000 etc.) */
};

int      s_watch_active = 0;
int      s_watch_nranges = 0;
emu_watch_range s_watch_ranges[EMU_WATCH_MAX_RANGES] = {};
uint32_t s_watch_frame = 0;
int      s_watch_write_idx = 0;
int      s_watch_count = 0;
emu_watch_entry s_watch_log[EMU_WATCH_LOG_SIZE] = {};

/* ---- Per-frame WRAM history ring (snes9x analog of recomp's
 * s_frame_history) ----
 *
 * Captures Memory.RAM[$0000-$1FFF] (bank 7E low WRAM = SMW's
 * gameplay state region) after each retro_run. Probes can query
 * "what was emu's $D3 (Mario Y) at the frame where emu's $D1
 * (Mario X) was just written to value X?" — equivalent to recomp's
 * existing FrameRecord lookups.
 *
 * Sized to match recomp's history ring (6000 frames). 8KB per
 * frame × 6000 frames = ~48 MB resident — acceptable for the
 * Oracle build (debug-only). Release|x64 doesn't link the bridge,
 * so this is not in the shipping exe.
 *
 * Larger slices (full bank 7E + bank 7F = 128KB/frame × 6000 =
 * 768MB) are intentionally not captured here; a separate query
 * shape can use the always-on WRAM trace ring for higher-address
 * writes if needed.
 */
#define EMU_FRAME_HIST_SIZE   6000
#define EMU_FRAME_HIST_SLICE  0x2000   /* bank 7E $0000-$1FFF */

struct emu_frame_history_entry {
    uint32_t frame;                          /* s_watch_frame at capture */
    uint8_t  wram[EMU_FRAME_HIST_SLICE];
};

static emu_frame_history_entry s_emu_frame_hist[EMU_FRAME_HIST_SIZE];
static int  s_emu_frame_hist_write_idx = 0;
static int  s_emu_frame_hist_count = 0;

/* Map an incoming 24-bit CPU bus address to a 20-bit WRAM offset if
 * it targets bank 7E/7F or a WRAM-mirror. Returns 0xFFFFFFFF if the
 * address is not WRAM. */
inline uint32_t bus_addr_to_wram_offset(uint32_t busaddr) {
    uint32_t bank = (busaddr >> 16) & 0xFF;
    uint32_t off  = busaddr & 0xFFFF;
    if (bank == 0x7E)                     return off;                 /* $7E:0000-$7EFFFF -> $00000-$0FFFF */
    if (bank == 0x7F)                     return 0x10000 + off;       /* $7F:0000-$7FFFFF -> $10000-$1FFFF */
    /* WRAM mirror at $00-$3F,$80-$BF : $0000-$1FFF */
    if (off <= 0x1FFF) {
        if (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) return off;
    }
    return 0xFFFFFFFFu;
}

void s9x_bridge_write_hook(uint32_t bus_addr, uint8_t byte) {
    if (!s_watch_active) return;
    uint32_t wram = bus_addr_to_wram_offset(bus_addr);
    if (wram == 0xFFFFFFFFu) return;
    int matched = 0;
    for (int i = 0; i < s_watch_nranges; i++)
        if (wram >= s_watch_ranges[i].lo && wram <= s_watch_ranges[i].hi) { matched = 1; break; }
    if (!matched) return;

    int idx = s_watch_write_idx % EMU_WATCH_LOG_SIZE;
    s_watch_log[idx].frame = s_watch_frame;
    s_watch_log[idx].addr  = wram;
    /* PC at hook time: snes9x advances PC past the opcode byte before
     * dispatch, so this is "just after the opcode fetch, during the
     * opcode body". Cross-reference SMWDisX by subtracting instruction
     * length to get pre-instruction PC, or just match the nearest
     * STA/STZ to this PC. */
    s_watch_log[idx].pc24 = ((uint32_t)Registers.PC.B.xPB << 16) | Registers.PC.W.xPC;
    s_watch_log[idx].byte_before = Memory.RAM[wram];  /* pre-write value */
    s_watch_log[idx].byte_after  = byte;
    s_watch_log[idx].bank_source = (uint16_t)((bus_addr >> 16) & 0xFF);
    s_watch_write_idx++;
    if (s_watch_count < EMU_WATCH_LOG_SIZE) s_watch_count++;
}

/* ---- Libretro callbacks ---- */

/* Video: copy whatever snes9x hands us. Oracle build has no
 * on-screen window for the embedded emu — we're here for state,
 * not pixels. Frame buffer is kept in case emu_screenshot lands
 * in a later tier. */
void retro_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) return;
    s_frame_width  = width;
    s_frame_height = height;
    /* Convert the core's framebuffer to XRGB8888 honoring the requested
     * pixel format (snes9x emits RGB565 by default). `pitch` is in bytes. */
    unsigned copy_h = height > 240 ? 240 : height;
    unsigned copy_w = width > 256 ? 256 : width;
    if (s_pixel_format == 1) {
        /* XRGB8888: direct copy. */
        for (unsigned y = 0; y < copy_h; y++)
            memcpy(s_framebuf_xrgb + y * 256,
                   (const uint8_t *)data + y * pitch,
                   copy_w * sizeof(uint32_t));
    } else {
        /* 16-bit source (RGB565 or 0RGB1555). Expand each pixel to
         * 0x00RRGGBB so the screenshot writer (which reads bytes B,G,R)
         * gets correct colours. */
        const int is565 = (s_pixel_format == 2);
        for (unsigned y = 0; y < copy_h; y++) {
            const uint16_t *srow = (const uint16_t *)((const uint8_t *)data + y * pitch);
            uint32_t *drow = s_framebuf_xrgb + y * 256;
            for (unsigned x = 0; x < copy_w; x++) {
                uint16_t p = srow[x];
                uint8_t r, g, b;
                if (is565) {
                    r = (uint8_t)((p >> 11) & 0x1F); r = (uint8_t)((r << 3) | (r >> 2));
                    g = (uint8_t)((p >> 5)  & 0x3F); g = (uint8_t)((g << 2) | (g >> 4));
                    b = (uint8_t)( p        & 0x1F); b = (uint8_t)((b << 3) | (b >> 2));
                } else { /* 0RGB1555 */
                    r = (uint8_t)((p >> 10) & 0x1F); r = (uint8_t)((r << 3) | (r >> 2));
                    g = (uint8_t)((p >> 5)  & 0x1F); g = (uint8_t)((g << 3) | (g >> 2));
                    b = (uint8_t)( p        & 0x1F); b = (uint8_t)((b << 3) | (b >> 2));
                }
                drow[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
    }
}

void retro_audio_sample(int16_t left, int16_t right) {
    (void)left; (void)right;
}

size_t retro_audio_sample_batch(const int16_t *data, size_t frames) {
    (void)data;
    return frames;
}

void retro_input_poll(void) {
    /* Driven entirely from s_joypad[] — the recomp-side input is
     * captured once per frame and handed to us by snes9x_bridge_run_frame. */
}

/* Map the SNES hardware joypad bit order (used by the recomp runner)
 * to libretro's RETRO_DEVICE_ID_JOYPAD_* ids.
 *
 * SMW runner's joypad word layout (matches $4218 lo byte + $4219 hi byte):
 *   bit 15 B       bit 7  A
 *   bit 14 Y       bit 6  X
 *   bit 13 SELECT  bit 5  L
 *   bit 12 START   bit 4  R
 *   bit 11 UP
 *   bit 10 DOWN
 *   bit  9 LEFT
 *   bit  8 RIGHT
 */
int16_t retro_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)device; (void)index;
    if (port > 1) return 0;
    uint16_t j = s_joypad[port];
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B:      return (j >> 15) & 1;
        case RETRO_DEVICE_ID_JOYPAD_Y:      return (j >> 14) & 1;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return (j >> 13) & 1;
        case RETRO_DEVICE_ID_JOYPAD_START:  return (j >> 12) & 1;
        case RETRO_DEVICE_ID_JOYPAD_UP:     return (j >> 11) & 1;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (j >> 10) & 1;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (j >> 9)  & 1;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (j >> 8)  & 1;
        case RETRO_DEVICE_ID_JOYPAD_A:      return (j >> 7)  & 1;
        case RETRO_DEVICE_ID_JOYPAD_X:      return (j >> 6)  & 1;
        case RETRO_DEVICE_ID_JOYPAD_L:      return (j >> 5)  & 1;
        case RETRO_DEVICE_ID_JOYPAD_R:      return (j >> 4)  & 1;
        default: return 0;
    }
}

bool retro_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            *(const char **)data = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            /* Record the requested format so retro_video_refresh converts
             * correctly (snes9x requests RGB565). */
            if (data) s_pixel_format = *(const unsigned *)data;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *)data;
            var->value = nullptr;
            return false;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            /* No log callback — silence snes9x. */
            return false;
        }
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            return false;
        default:
            return false;
    }
}

/* Slurp ROM file fully into memory; snes9x libretro accepts it via
 * game->data/size and does its own LoROM/HiROM detection. */
bool load_rom_bytes(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t n = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return n == (size_t)sz;
}

std::vector<uint8_t> s_rom_bytes;  /* kept alive for retro_load_game's lifetime */

} /* anonymous namespace */

/* The hook pointer snes9x's getset.h references as extern. NULL
 * until emu_wram_trace is activated; then points at our anon-ns
 * hook function via a thin C-linkage trampoline below. */
extern "C" void (*s9x_write_hook)(uint32_t Address, uint8_t Byte) = nullptr;

/* Trampoline so the anonymous-namespace hook (which closes over
 * static state) can be installed as a plain C function pointer. */
extern "C" void s9x_write_hook_trampoline(uint32_t a, uint8_t b) {
    s9x_bridge_write_hook(a, b);
}

/* ---- Per-instruction trace ---- */
/* Captures full hardware register state at every CPU instruction
 * dispatch. Fills the gap that recomp's symbolic tracker can only
 * provide A/X/Y/B — the hardware always knows the truth.
 *
 * Heap-allocated; capacity set by snes9x_bridge_insn_trace_alloc()
 * from SNESRECOMP_EMU_INSN_RING_ENTRIES env (decimal, clamped
 * [1<<16, 1<<28]). Default 64M entries × 24 bytes ≈ 1.5 GB, covering
 * ~2000 frames at typical attract-demo rates. Static-arrayed at 1M
 * (the previous default) covered ~100 frames, which forced "probe
 * quickly" workflows after every relaunch — the anti-pattern this
 * project rejects.
 */
#define EMU_INSN_TRACE_DEFAULT_ENTRIES (64u * 1024u * 1024u)

struct emu_insn_entry {
    int32_t  frame;
    uint32_t pc24;
    uint8_t  op;
    uint8_t  db;
    uint16_t a;
    uint16_t x;
    uint16_t y;
    uint16_t s;
    uint16_t d;
    uint16_t p_w;     /* 16-bit P (low = 6502 flags, high = m/x/e bits) */
    int32_t  cycles;
};

int               s_emu_insn_active = 0;
uint64_t          s_emu_insn_write_idx = 0;
uint64_t          s_emu_insn_count = 0;
uint64_t          s_emu_insn_capacity = 0;
emu_insn_entry   *s_emu_insn_trace = nullptr;

/* Heap-allocate the oracle insn ring. Honors SNESRECOMP_EMU_INSN_RING_ENTRIES
 * env override. Idempotent — second call frees + reallocs. Returns the
 * chosen capacity so callers can log it. */
extern "C" uint64_t snes9x_bridge_insn_trace_alloc(void) {
    uint64_t cap = EMU_INSN_TRACE_DEFAULT_ENTRIES;
    const char *env = getenv("SNESRECOMP_EMU_INSN_RING_ENTRIES");
    if (env && *env) {
        unsigned long long v = strtoull(env, NULL, 0);
        if (v >= (1ULL << 16) && v <= (1ULL << 28)) cap = (uint64_t)v;
    }
    if (s_emu_insn_trace) free(s_emu_insn_trace);
    s_emu_insn_trace = (emu_insn_entry *)calloc((size_t)cap, sizeof(emu_insn_entry));
    s_emu_insn_capacity = s_emu_insn_trace ? cap : 0;
    s_emu_insn_write_idx = 0;
    s_emu_insn_count = 0;
    return cap;
}

/* NMI counter — ticks every NMI dispatch. Useful for "how many NMIs
 * fired between block_idx X and Y" cadence comparisons. */
uint64_t s_emu_nmi_count = 0;
int      s_emu_last_nmi_frame = -1;

/* Function-boundary WRAM snapshot history. Mirrors recomp's
 * g_recomp_snap_ring: captures the LOW 8KB of Memory.RAM into a
 * 256-deep ring buffer at every dispatch where PB:PC == s_emu_snap_pc24.
 * Each slot has the absolute call index + frame at capture. */
#define EMU_SNAP_SLICE_LEN  0x2000
#define EMU_SNAP_RING_LEN   256

uint32_t s_emu_snap_pc24    = 0;  /* 0 = disabled */
int      s_emu_snap_count   = 0;
int      s_emu_snap_frame   = -1;

struct emu_snap_entry {
    int     call_idx;
    int     frame;
    uint8_t wram_slice[EMU_SNAP_SLICE_LEN];
};
emu_snap_entry s_emu_snap_ring[EMU_SNAP_RING_LEN];

/* Public lookup, called from the TCP command. Returns nullptr if
 * the requested call_idx is no longer in the ring window. */
extern "C" const emu_snap_entry* emu_snap_lookup(int call_idx) {
    if (call_idx < 1) return nullptr;
    int slot = (call_idx - 1) % EMU_SNAP_RING_LEN;
    if (s_emu_snap_ring[slot].call_idx != call_idx) return nullptr;
    return &s_emu_snap_ring[slot];
}

/* ---- Oracle GM14 per-tick player-state trace ----
 *
 * Mirrors the recomp-side g_gm14_trace_ring (cpu_trace.c). Captures one
 * row each time the oracle CPU enters $00:C47E (GameMode14_InLevel).
 * Reads field bytes directly from Memory.RAM (snes9x's bank-7E low WRAM).
 * Always-on: no arming required; the row is captured every time PB:PC
 * hits the entry, from the moment the bridge is loaded.
 *
 * Entry-only for Step 2a — exit detection deferred (see Step 2 plan).
 */
#define EMU_GM14_DEFAULT_ENTRIES (1u << 17)
#define EMU_GM14_TARGET_PC24      0x00C47Eu
#define EMU_GM14_TARGET_PB        0x00u
#define EMU_GM14_TARGET_PC        0xC47Eu

struct emu_gm14_row {
    uint64_t tick_ordinal;
    int32_t  oracle_frame;
    uint8_t  kind;             /* 0 = ENTRY (only kind populated for now) */
    uint8_t  gamemode;
    uint8_t  in_15, in_16, in_17, in_18;
    uint8_t  st_71, st_77;
    int8_t   xspeed, yspeed;
    uint16_t pos_x, pos_y;
    uint8_t  yoshi_187A, yoshi_18E2, yoshi_1888;
    uint8_t  scroll_1A, scroll_1C;
    uint8_t  pad0;
    uint16_t cam_1462, cam_1464;
    uint8_t  spr9_status, spr9_number;
    uint16_t spr9_x, spr9_y;
    /* Oracle CPU regs at entry — useful when divergence is in inputs/etc.
     * Field names prefixed `r_` to avoid clashing with snes9x's 65c816.h
     * macros (e.g. `PB` expands to `PC.B.xPB`). */
    uint16_t r_A, r_X, r_Y, r_S, r_D;
    uint8_t  r_DB, r_PB;
    uint16_t r_P;
};

static emu_gm14_row *s_emu_gm14_ring = nullptr;
static uint64_t      s_emu_gm14_capacity = 0;
static uint64_t      s_emu_gm14_idx = 0;
static uint64_t      s_emu_gm14_tick_ordinal = 0;

static uint64_t emu_gm14_round_pow2_down(uint64_t v) {
    if (v == 0) return 0;
    uint64_t p = 1;
    while ((p << 1) && (p << 1) <= v) p <<= 1;
    return p;
}

extern "C" uint64_t snes9x_bridge_gm14_init(void) {
    uint64_t cap = EMU_GM14_DEFAULT_ENTRIES;
    const char *env = getenv("SNESRECOMP_EMU_GM14_RING_ENTRIES");
    if (env && *env) {
        unsigned long long v = strtoull(env, NULL, 0);
        if (v >= (1ULL << 14) && v <= (1ULL << 22)) cap = (uint64_t)v;
    }
    cap = emu_gm14_round_pow2_down(cap);
    if (cap < (1ULL << 14)) cap = (1ULL << 14);
    if (s_emu_gm14_ring) free(s_emu_gm14_ring);
    s_emu_gm14_ring = (emu_gm14_row *)calloc((size_t)cap, sizeof(emu_gm14_row));
    s_emu_gm14_capacity = s_emu_gm14_ring ? cap : 0;
    s_emu_gm14_idx = 0;
    s_emu_gm14_tick_ordinal = 0;
    return s_emu_gm14_capacity;
}

extern "C" void snes9x_bridge_gm14_clear(void) {
    s_emu_gm14_idx = 0;
    s_emu_gm14_tick_ordinal = 0;
    if (s_emu_gm14_ring && s_emu_gm14_capacity) {
        memset(s_emu_gm14_ring, 0,
               (size_t)s_emu_gm14_capacity * sizeof(emu_gm14_row));
    }
}

extern "C" uint64_t snes9x_bridge_gm14_idx(void)        { return s_emu_gm14_idx; }
extern "C" uint64_t snes9x_bridge_gm14_capacity(void)   { return s_emu_gm14_capacity; }
extern "C" uint64_t snes9x_bridge_gm14_tick_ordinal(void){ return s_emu_gm14_tick_ordinal; }

/* Read a row by absolute idx. Returns 1 if the requested idx is still
 * resident in the ring window, 0 otherwise. Caller passes a buffer
 * matching the row layout (see emu_gm14_row). */
extern "C" int snes9x_bridge_gm14_get_row(uint64_t abs_idx, void *out_row) {
    if (!s_emu_gm14_ring || !s_emu_gm14_capacity) return 0;
    if (abs_idx >= s_emu_gm14_idx) return 0;
    if (s_emu_gm14_idx > s_emu_gm14_capacity &&
        abs_idx < (s_emu_gm14_idx - s_emu_gm14_capacity)) {
        return 0;
    }
    emu_gm14_row *r = &s_emu_gm14_ring[abs_idx & (s_emu_gm14_capacity - 1)];
    memcpy(out_row, r, sizeof(*r));
    return 1;
}

/* ── Oracle-side block-keyed sampler ──────────────────────────────────
 * Same shape as recomp's BlockWatch (cpu_trace.h). Captures registers +
 * specified WRAM bytes on every entry to the configured PB:PC. */
#define EMU_BLOCK_WATCH_MAX        16
#define EMU_BLOCK_WATCH_ADDRS_MAX  8
#define EMU_BLOCK_WATCH_HITS_MAX   256

struct emu_block_watch_hit {
    int32_t  frame;
    uint16_t r_A, r_X, r_Y, r_S, r_D;
    uint8_t  r_DB, r_PB;
    uint16_t r_P;
    uint32_t r_ret;   /* 24-bit return address read off the stack at entry
                       * ($00:[S+1..S+3]); caller = r_ret-2 (JSR) or -3 (JSL).
                       * Only meaningful when the watched PC is a subroutine
                       * entry (no pushes precede it). */
    uint8_t  vals[EMU_BLOCK_WATCH_ADDRS_MAX];
};

struct emu_block_watch {
    uint8_t  enabled;
    uint32_t pc24;
    int      n_addrs;
    int32_t  ram_offsets[EMU_BLOCK_WATCH_ADDRS_MAX];
    int      max_hits;
    int      hit_count;
    emu_block_watch_hit hits[EMU_BLOCK_WATCH_HITS_MAX];
};

static emu_block_watch s_emu_block_watches[EMU_BLOCK_WATCH_MAX] = {};
static uint8_t         s_emu_block_watch_any = 0;

extern "C" void snes9x_bridge_block_watch_arm(uint32_t pc24,
                                                const int32_t *offs,
                                                int n_addrs,
                                                int max_hits) {
    if (n_addrs < 0) n_addrs = 0;
    if (n_addrs > EMU_BLOCK_WATCH_ADDRS_MAX) n_addrs = EMU_BLOCK_WATCH_ADDRS_MAX;
    if (max_hits < 1) max_hits = 1;
    if (max_hits > EMU_BLOCK_WATCH_HITS_MAX) max_hits = EMU_BLOCK_WATCH_HITS_MAX;
    int slot = -1;
    for (int i = 0; i < EMU_BLOCK_WATCH_MAX; i++) {
        if (s_emu_block_watches[i].enabled &&
            s_emu_block_watches[i].pc24 == pc24) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < EMU_BLOCK_WATCH_MAX; i++) {
            if (!s_emu_block_watches[i].enabled) { slot = i; break; }
        }
    }
    if (slot < 0) {
        fprintf(stderr, "[snes9x] emu_block_watch table FULL (max=%d)\n",
                EMU_BLOCK_WATCH_MAX);
        return;
    }
    emu_block_watch *w = &s_emu_block_watches[slot];
    memset(w, 0, sizeof(*w));
    w->enabled = 1;
    w->pc24 = pc24;
    w->n_addrs = n_addrs;
    for (int i = 0; i < EMU_BLOCK_WATCH_ADDRS_MAX; i++) {
        w->ram_offsets[i] = (i < n_addrs) ? offs[i] : -1;
    }
    w->max_hits = max_hits;
    w->hit_count = 0;
    s_emu_block_watch_any = 1;
}

extern "C" void snes9x_bridge_block_watch_clear_all(void) {
    memset(s_emu_block_watches, 0, sizeof(s_emu_block_watches));
    s_emu_block_watch_any = 0;
}

extern "C" void snes9x_bridge_block_watch_clear_one(int slot) {
    if (slot < 0 || slot >= EMU_BLOCK_WATCH_MAX) return;
    memset(&s_emu_block_watches[slot], 0, sizeof(emu_block_watch));
    int any = 0;
    for (int i = 0; i < EMU_BLOCK_WATCH_MAX; i++) {
        if (s_emu_block_watches[i].enabled) { any = 1; break; }
    }
    s_emu_block_watch_any = any ? 1 : 0;
}

extern "C" int snes9x_bridge_block_watch_count(void) {
    int n = 0;
    for (int i = 0; i < EMU_BLOCK_WATCH_MAX; i++) {
        if (s_emu_block_watches[i].enabled) n++;
    }
    return n;
}

/* Slot accessor — returns 0 if `slot` is empty/disabled, 1 otherwise.
 * Caller's pointers are populated only on success. */
extern "C" int snes9x_bridge_block_watch_get_meta(int slot,
                                                    uint32_t *out_pc24,
                                                    int *out_n_addrs,
                                                    int *out_max_hits,
                                                    int *out_hit_count,
                                                    int32_t out_addrs[EMU_BLOCK_WATCH_ADDRS_MAX]) {
    if (slot < 0 || slot >= EMU_BLOCK_WATCH_MAX) return 0;
    emu_block_watch *w = &s_emu_block_watches[slot];
    if (!w->enabled) return 0;
    if (out_pc24)      *out_pc24      = w->pc24;
    if (out_n_addrs)   *out_n_addrs   = w->n_addrs;
    if (out_max_hits)  *out_max_hits  = w->max_hits;
    if (out_hit_count) *out_hit_count = w->hit_count;
    if (out_addrs) {
        for (int i = 0; i < EMU_BLOCK_WATCH_ADDRS_MAX; i++) out_addrs[i] = w->ram_offsets[i];
    }
    return 1;
}

extern "C" int snes9x_bridge_block_watch_get_hit(int slot, int hit,
                                                   int32_t *out_frame,
                                                   uint16_t out_regs[8],
                                                   uint8_t  out_vals[EMU_BLOCK_WATCH_ADDRS_MAX]) {
    if (slot < 0 || slot >= EMU_BLOCK_WATCH_MAX) return 0;
    emu_block_watch *w = &s_emu_block_watches[slot];
    if (!w->enabled || hit < 0 || hit >= w->hit_count) return 0;
    emu_block_watch_hit *h = &w->hits[hit];
    if (out_frame) *out_frame = h->frame;
    if (out_regs) {
        out_regs[0] = h->r_A; out_regs[1] = h->r_X; out_regs[2] = h->r_Y;
        out_regs[3] = h->r_S; out_regs[4] = h->r_D;
        out_regs[5] = (uint16_t)h->r_DB;
        out_regs[6] = (uint16_t)h->r_PB;
        out_regs[7] = h->r_P;
    }
    if (out_vals) {
        for (int i = 0; i < EMU_BLOCK_WATCH_ADDRS_MAX; i++) out_vals[i] = h->vals[i];
    }
    return 1;
}

/* Caller-context accessor: the 24-bit return address captured off the
 * stack at entry. Separate from get_hit so the existing wire format is
 * untouched. Returns 0 if slot/hit invalid. */
extern "C" int snes9x_bridge_block_watch_get_ret(int slot, int hit,
                                                   uint32_t *out_ret) {
    if (slot < 0 || slot >= EMU_BLOCK_WATCH_MAX) return 0;
    emu_block_watch *w = &s_emu_block_watches[slot];
    if (!w->enabled || hit < 0 || hit >= w->hit_count) return 0;
    if (out_ret) *out_ret = w->hits[hit].r_ret;
    return 1;
}

void s9x_bridge_insn_hook(uint8_t pb, uint16_t pc, uint8_t op) {
    /* Oracle GM14 entry-only trace. Fires every time PB:PC == $00:C47E.
     * No call-depth/return tracking needed for entry-only first pass. */
    if (s_emu_gm14_ring && s_emu_gm14_capacity &&
        pb == EMU_GM14_TARGET_PB && pc == EMU_GM14_TARGET_PC) {
        s_emu_gm14_tick_ordinal++;
        uint64_t slot = s_emu_gm14_idx & (s_emu_gm14_capacity - 1);
        emu_gm14_row *r = &s_emu_gm14_ring[slot];
        const uint8_t *RAM = Memory.RAM;
        r->tick_ordinal = s_emu_gm14_tick_ordinal;
        r->oracle_frame = (int32_t)s_watch_frame;
        r->kind         = 0;  /* ENTRY */
        r->gamemode     = RAM[0x0100];
        r->in_15        = RAM[0x0015];
        r->in_16        = RAM[0x0016];
        r->in_17        = RAM[0x0017];
        r->in_18        = RAM[0x0018];
        r->st_71        = RAM[0x0071];
        r->st_77        = RAM[0x0077];
        r->xspeed       = (int8_t)RAM[0x007B];
        r->yspeed       = (int8_t)RAM[0x007D];
        r->pos_x        = (uint16_t)(RAM[0x0094] | ((uint16_t)RAM[0x0095] << 8));
        r->pos_y        = (uint16_t)(RAM[0x0096] | ((uint16_t)RAM[0x0097] << 8));
        r->yoshi_187A   = RAM[0x187A];
        r->yoshi_18E2   = RAM[0x18E2];
        r->yoshi_1888   = RAM[0x1888];
        r->scroll_1A    = RAM[0x001A];
        r->scroll_1C    = RAM[0x001C];
        r->pad0         = 0;
        r->cam_1462     = (uint16_t)(RAM[0x1462] | ((uint16_t)RAM[0x1463] << 8));
        r->cam_1464     = (uint16_t)(RAM[0x1464] | ((uint16_t)RAM[0x1465] << 8));
        r->spr9_status  = RAM[0x14C8 + 9];
        r->spr9_number  = RAM[0x009E + 9];
        r->spr9_x       = (uint16_t)(RAM[0x00E4 + 9] | ((uint16_t)RAM[0x14E0 + 9] << 8));
        r->spr9_y       = (uint16_t)(RAM[0x00D8 + 9] | ((uint16_t)RAM[0x14D4 + 9] << 8));
        r->r_A          = Registers.A.W;
        r->r_X          = Registers.X.W;
        r->r_Y          = Registers.Y.W;
        r->r_S          = Registers.S.W;
        r->r_D          = Registers.D.W;
        r->r_DB         = Registers.DB;
        r->r_PB         = Registers.PB;
        r->r_P          = (uint16_t)Registers.P.W;
        s_emu_gm14_idx++;
    }

    /* ── Oracle-side block-keyed sampler ──────────────────────────────
     *
     * Mirrors the recomp-side cpu_trace_block_watch_check. Lets a single
     * Python differ ask the same question on both sides: at PB:PC, what
     * are the WRAM bytes at these offsets? */
    if (s_emu_block_watch_any) {
        uint32_t cur_pc24 = ((uint32_t)pb << 16) | pc;
        for (int i = 0; i < EMU_BLOCK_WATCH_MAX; i++) {
            emu_block_watch *w = &s_emu_block_watches[i];
            if (!w->enabled || w->pc24 != cur_pc24) continue;
            if (w->hit_count >= w->max_hits) continue;
            emu_block_watch_hit *h = &w->hits[w->hit_count];
            h->frame = (int32_t)s_watch_frame;
            h->r_A   = Registers.A.W;
            h->r_X   = Registers.X.W;
            h->r_Y   = Registers.Y.W;
            h->r_S   = Registers.S.W;
            h->r_D   = Registers.D.W;
            h->r_DB  = Registers.DB;
            h->r_PB  = Registers.PB;
            h->r_P   = (uint16_t)Registers.P.W;
            {   /* return address sitting on the stack at subroutine entry */
                uint16_t s = Registers.S.W;
                h->r_ret = (uint32_t)Memory.RAM[0x0100 | ((s + 1) & 0xFF)]
                         | ((uint32_t)Memory.RAM[0x0100 | ((s + 2) & 0xFF)] << 8)
                         | ((uint32_t)Memory.RAM[0x0100 | ((s + 3) & 0xFF)] << 16);
            }
            for (int j = 0; j < w->n_addrs; j++) {
                int32_t off = w->ram_offsets[j];
                h->vals[j] = (off >= 0 && off < 0x20000) ? Memory.RAM[off] : 0;
            }
            w->hit_count++;
            break;
        }
    }

    /* Function-boundary snapshot fires regardless of insn-trace state. */
    if (s_emu_snap_pc24 != 0) {
        uint32_t cur = ((uint32_t)pb << 16) | pc;
        if (cur == s_emu_snap_pc24) {
            s_emu_snap_count++;
            s_emu_snap_frame = (int)s_watch_frame;
            int slot = (s_emu_snap_count - 1) % EMU_SNAP_RING_LEN;
            s_emu_snap_ring[slot].call_idx = s_emu_snap_count;
            s_emu_snap_ring[slot].frame    = (int)s_watch_frame;
            memcpy(s_emu_snap_ring[slot].wram_slice, Memory.RAM, EMU_SNAP_SLICE_LEN);
        }
    }
    if (!s_emu_insn_active || !s_emu_insn_trace || s_emu_insn_capacity == 0)
        return;
    uint64_t idx = s_emu_insn_write_idx % s_emu_insn_capacity;
    auto &e = s_emu_insn_trace[idx];
    e.frame = (int32_t)s_watch_frame;
    e.pc24  = ((uint32_t)pb << 16) | pc;
    e.op    = op;
    e.db    = Registers.DB;
    e.a     = Registers.A.W;
    e.x     = Registers.X.W;
    e.y     = Registers.Y.W;
    e.s     = Registers.S.W;
    e.d     = Registers.D.W;
    e.p_w   = (uint16_t)Registers.P.W;
    e.cycles = CPU.Cycles;
    s_emu_insn_write_idx++;
    if (s_emu_insn_count < s_emu_insn_capacity) s_emu_insn_count++;
}

void s9x_bridge_nmi_hook(void) {
    s_emu_nmi_count++;
    s_emu_last_nmi_frame = (int32_t)s_watch_frame;
}

/* ---- Public C API ---- */

extern "C" {

int snes9x_bridge_init(const char *rom_path) {
    if (s_loaded) return 0;

    retro_set_environment(retro_environment);
    retro_set_video_refresh(retro_video_refresh);
    retro_set_audio_sample(retro_audio_sample);
    retro_set_audio_sample_batch(retro_audio_sample_batch);
    retro_set_input_poll(retro_input_poll);
    retro_set_input_state(retro_input_state);

    retro_init();

    if (!load_rom_bytes(rom_path, s_rom_bytes)) {
        fprintf(stderr, "[snes9x] could not read ROM: %s\n", rom_path);
        retro_deinit();
        return -1;
    }

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = rom_path;
    info.data = s_rom_bytes.data();
    info.size = s_rom_bytes.size();

    if (!retro_load_game(&info)) {
        fprintf(stderr, "[snes9x] retro_load_game failed: %s\n", rom_path);
        retro_deinit();
        s_rom_bytes.clear();
        return -2;
    }

    s_loaded = true;
    fprintf(stderr, "[snes9x] Oracle backend loaded (%zu bytes): %s\n",
            s_rom_bytes.size(), rom_path);

    // Always-on WRAM trace: arm the s9x write-hook for the full 128 KB
    // WRAM range BEFORE the first retro_run() so every store from
    // snes9x's reset/boot sequence onward is recorded continuously.
    // Probes query the ring backward in history and never need to
    // arm-then-record (which loses early writes to attach latency).
    extern int snes9x_bridge_watch_add(uint32_t lo, uint32_t hi);
    snes9x_bridge_watch_add(0x00000, 0x1FFFF);
    fprintf(stderr, "[snes9x] always-on WRAM trace armed for full $0..$1FFFF range\n");

    // Always-on per-instruction trace. Mirrors the WRAM trace above:
    // probes query the ring backward in history rather than arm-then-
    // record. emu_insn_trace_on / _off remain available for explicit
    // toggling, but the default is armed so any probe — including the
    // ring-driven differ — finds populated data the moment it attaches.
    {
        uint64_t cap = snes9x_bridge_insn_trace_alloc();
        if (s_emu_insn_trace) {
            fprintf(stderr,
                "[snes9x] insn ring allocated: %llu entries (~%llu MB)\n",
                (unsigned long long)cap,
                (unsigned long long)((cap * sizeof(emu_insn_entry)) >> 20));
        } else {
            fprintf(stderr,
                "[snes9x] WARNING: failed to allocate insn ring "
                "(%llu entries); reduce SNESRECOMP_EMU_INSN_RING_ENTRIES\n",
                (unsigned long long)cap);
        }
    }
    snes9x_bridge_insn_trace_on();
    fprintf(stderr, "[snes9x] always-on insn trace armed\n");

    /* Always-on oracle GM14 player-state trace ring. Captures one row
     * per oracle-side entry to $00:C47E (GameMode14_InLevel) so a
     * recomp-vs-oracle differ can pair by tick_ordinal. */
    {
        uint64_t cap = snes9x_bridge_gm14_init();
        fprintf(stderr,
            "[snes9x] gm14 player-trace ring allocated: %llu entries (~%llu MB)%s\n",
            (unsigned long long)cap,
            (unsigned long long)((cap * sizeof(emu_gm14_row)) >> 20),
            s_emu_gm14_ring ? "" : " — ALLOC FAILED");
    }

    // Always-on VRAM byte-write trace. The recompiler-side ring + the
    // oracle-side ring together let cmd_vram_write_diff mechanically
    // identify the first divergent (addr, byte) pair across the two
    // write streams without anyone hand-reading SMWDisX.
    snes9x_bridge_vram_hook_arm();
    fprintf(stderr, "[snes9x] always-on VRAM-write trace armed\n");

    return 0;
}

/* Yield-on-logical-frame wrapper around retro_run.
 *
 * Recomp's "1 step" = 1 SmwRunOneFrameOfGame call = 1 NMI body + 1
 * main-loop body iteration = 1 SMW logical frame.
 *
 * Snes9x's retro_run = 1 wall-clock 60Hz frame of cycles. During
 * NMI-enabled stages this captures 1 NMI-driven RunGameMode call,
 * same logical unit as recomp. During NMI-disabled stages (GM=$03,
 * $04 boot uploads) the handler body executes inline without NMI;
 * one retro_run captures only a fraction of one such body, taking
 * 20-55 retro_runs to traverse a single handler invocation. That
 * mismatch means lockstep state-sync vs recomp is invalid in those
 * stages — recomp completes the body in 1 step.
 *
 * Fix: loop retro_run until ONE of:
 *   1. NMI fires (s_emu_nmi_count increments) — covers NMI-enabled
 *      stages; matches recomp's 1-NMI-per-step.
 *   2. GameMode ($0100) changes — covers NMI-disabled stages where
 *      the handler INC's GameMode at completion.
 *   3. Max iterations reached (escape hatch for genuinely stuck
 *      states; 240 = 4 wall-clock seconds is well past any boot
 *      stage's natural completion time).
 *
 * Result: per-step semantics are "1 SMW logical-frame iteration of
 * progress" on both sides regardless of NMI state.
 *
 * The wall-clock-frame counter `s_watch_frame` still increments per
 * retro_run call to keep the trace ring's `f` field meaningful as
 * "wall-clock frames of cycles consumed since boot." Probes that
 * compare logical-frame counts use rec_steps and the GameMode
 * transition log; probes that need wall-clock cycles use s_watch_frame.
 */
void snes9x_bridge_run_frame(uint16_t joypad1, uint16_t joypad2) {
    if (!s_loaded) return;
    s_joypad[0] = joypad1;
    s_joypad[1] = joypad2;
    /* Snapshot WRAM (full 128KB) so emu_wram_delta can report what
     * the LOGICAL frame changed — captured before the loop, not
     * before each retro_run, so the delta covers the full unit. */
    memcpy(s_wram_before, Memory.RAM, 0x20000);

    uint64_t nmi_before = s_emu_nmi_count;

    /* MAX_RETRO_RUNS budget. SMW's GM=$04 (PrepareTitleScreen)
     * takes ~55 wall-clock frames on snes9x with NMI disabled.
     * Cap at 80 to give headroom without ballooning probe wall-
     * clock time. NMI-enabled stages exit after 1 retro_run.
     *
     * NMI-disabled stages will exhaust the budget without firing
     * NMI — accepted; snes9x doesn't fully traverse the long
     * handler body in one step, but the next step continues from
     * where this left off. Total step count to traverse the stage
     * still doesn't exactly match recomp (which does the body
     * in 1 step) but is closer than the original 1-retro_run
     * model.
     *
     * Note we don't watch $0100 (GameMode) for transition signals
     * because $0100 is on the stack page and gets clobbered by
     * stack pushes — produces spurious GM=$34/$00/etc. transitions
     * mid-handler. NMI is the clean signal. */
    enum { MAX_RETRO_RUNS = 80 };
    for (int i = 0; i < MAX_RETRO_RUNS; i++) {
        s_watch_frame++;
        retro_run();
        /* Capture into the per-frame history ring AFTER each
         * retro_run so the slice reflects post-frame state. */
        {
            int slot = s_emu_frame_hist_write_idx % EMU_FRAME_HIST_SIZE;
            s_emu_frame_hist[slot].frame = s_watch_frame;
            memcpy(s_emu_frame_hist[slot].wram, Memory.RAM, EMU_FRAME_HIST_SLICE);
            s_emu_frame_hist_write_idx++;
            if (s_emu_frame_hist_count < EMU_FRAME_HIST_SIZE)
                s_emu_frame_hist_count++;
        }
        if (s_emu_nmi_count != nmi_before) break;          /* NMI fired */
    }
}

/* Public C accessors for the per-frame history. Returns 0 on
 * miss, 1 on hit. addr is bank-7E offset (0..$1FFF). */
extern "C" int snes9x_bridge_history_count(void) {
    return s_emu_frame_hist_count;
}

extern "C" int snes9x_bridge_screenshot(const char *path) {
    if (!path || !*path) path = "emu_screenshot.bmp";
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    int w = (int)(s_frame_width > 256 ? 256 : s_frame_width);
    int h = (int)(s_frame_height > 240 ? 240 : s_frame_height);
    if (w <= 0) w = 256;
    if (h <= 0) h = 224;

    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;
    int img_size = stride * h;
    int file_size = 54 + img_size;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size; hdr[3] = file_size >> 8;
    hdr[4] = file_size >> 16; hdr[5] = file_size >> 24;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w; hdr[19] = w >> 8;
    int neg_h = -h;
    memcpy(&hdr[22], &neg_h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = img_size; hdr[35] = img_size >> 8;
    hdr[36] = img_size >> 16; hdr[37] = img_size >> 24;
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t row_buf[256 * 3 + 4];
    memset(row_buf, 0, sizeof(row_buf));
    for (int y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)(s_framebuf_xrgb + y * 256);
        for (int x = 0; x < w; x++) {
            row_buf[x * 3 + 0] = src[x * 4 + 0];
            row_buf[x * 3 + 1] = src[x * 4 + 1];
            row_buf[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row_buf, 1, stride, f);
    }

    fclose(f);
    return 1;
}

/* Write a byte to snes9x's WRAM at the given offset (0..$1FFFF).
 * Used by input-injection / state-injection probes that need to
 * synchronize Mario's position on both sides for collision-physics
 * comparison. */
extern "C" int snes9x_bridge_write_wram(uint32_t offset, uint8_t val) {
    if (offset >= 0x20000) return 0;
    Memory.RAM[offset] = val;
    return 1;
}
extern "C" int snes9x_bridge_history_oldest_frame(void) {
    if (s_emu_frame_hist_count == 0) return -1;
    int start = (s_emu_frame_hist_write_idx - s_emu_frame_hist_count
                 + EMU_FRAME_HIST_SIZE) % EMU_FRAME_HIST_SIZE;
    return (int)s_emu_frame_hist[start].frame;
}
extern "C" int snes9x_bridge_history_newest_frame(void) {
    if (s_emu_frame_hist_count == 0) return -1;
    int idx = (s_emu_frame_hist_write_idx - 1 + EMU_FRAME_HIST_SIZE)
              % EMU_FRAME_HIST_SIZE;
    return (int)s_emu_frame_hist[idx].frame;
}
extern "C" int snes9x_bridge_history_byte_at(uint32_t frame, uint32_t addr,
                                              uint8_t *out_val) {
    if (addr >= EMU_FRAME_HIST_SLICE) return 0;
    if (s_emu_frame_hist_count == 0) return 0;
    int start = (s_emu_frame_hist_write_idx - s_emu_frame_hist_count
                 + EMU_FRAME_HIST_SIZE) % EMU_FRAME_HIST_SIZE;
    for (int i = 0; i < s_emu_frame_hist_count; i++) {
        int idx = (start + i) % EMU_FRAME_HIST_SIZE;
        if (s_emu_frame_hist[idx].frame == frame) {
            if (out_val) *out_val = s_emu_frame_hist[idx].wram[addr];
            return 1;
        }
    }
    return 0;
}

extern "C" int snes9x_bridge_history_copy_range(uint32_t frame, uint32_t addr,
                                                 uint32_t len,
                                                 uint8_t *out) {
    if (!out || len == 0) return 0;
    if (addr >= EMU_FRAME_HIST_SLICE || addr + len > EMU_FRAME_HIST_SLICE) return 0;
    if (s_emu_frame_hist_count == 0) return 0;
    int start = (s_emu_frame_hist_write_idx - s_emu_frame_hist_count
                 + EMU_FRAME_HIST_SIZE) % EMU_FRAME_HIST_SIZE;
    for (int i = 0; i < s_emu_frame_hist_count; i++) {
        int idx = (start + i) % EMU_FRAME_HIST_SIZE;
        if (s_emu_frame_hist[idx].frame == frame) {
            memcpy(out, s_emu_frame_hist[idx].wram + addr, len);
            return 1;
        }
    }
    return 0;
}

/* Find the latest frame in history where wram[addr] matches val.
 * Returns the frame number, or -1 if not found. Useful for
 * waypoint queries: "what was the most recent frame where
 * Memory.RAM[$D1] == 0xC7?" */
extern "C" int snes9x_bridge_history_find_value(uint32_t addr, uint8_t val) {
    if (addr >= EMU_FRAME_HIST_SLICE) return -1;
    if (s_emu_frame_hist_count == 0) return -1;
    /* Walk newest-first so we return the most recent match. */
    for (int i = s_emu_frame_hist_count - 1; i >= 0; i--) {
        int idx = (s_emu_frame_hist_write_idx - 1 - (s_emu_frame_hist_count - 1 - i)
                   + EMU_FRAME_HIST_SIZE) % EMU_FRAME_HIST_SIZE;
        if (s_emu_frame_hist[idx].wram[addr] == val)
            return (int)s_emu_frame_hist[idx].frame;
    }
    return -1;
}

/* Find the latest frame in history where the 16-bit little-endian
 * word at wram[addr]:wram[addr+1] equals val. The common shape for
 * waypoint sync: SMW position fields are 16-bit (X/Y high/low). */
extern "C" int snes9x_bridge_history_find_word(uint32_t addr, uint16_t val) {
    if (addr + 1 >= EMU_FRAME_HIST_SLICE) return -1;
    if (s_emu_frame_hist_count == 0) return -1;
    uint8_t lo = (uint8_t)(val & 0xFF);
    uint8_t hi = (uint8_t)((val >> 8) & 0xFF);
    for (int i = s_emu_frame_hist_count - 1; i >= 0; i--) {
        int idx = (s_emu_frame_hist_write_idx - 1 - (s_emu_frame_hist_count - 1 - i)
                   + EMU_FRAME_HIST_SIZE) % EMU_FRAME_HIST_SIZE;
        if (s_emu_frame_hist[idx].wram[addr] == lo
            && s_emu_frame_hist[idx].wram[addr + 1] == hi)
            return (int)s_emu_frame_hist[idx].frame;
    }
    return -1;
}

/* Report bytes that changed in the most-recent retro_run(). out_buf
 * is the caller's scratch; out_caps bounds it. Each entry is
 * (uint32 addr, uint8 before, uint8 after) packed consecutively.
 * Returns number of entries written (may be clamped). */
int snes9x_bridge_get_wram_delta(uint32_t lo, uint32_t hi,
                                 uint32_t *out_addrs, uint8_t *out_before,
                                 uint8_t *out_after, int out_caps) {
    if (!s_loaded) return 0;
    if (lo > hi || hi >= 0x20000) return 0;
    int n = 0;
    for (uint32_t a = lo; a <= hi && n < out_caps; a++) {
        uint8_t b = s_wram_before[a];
        uint8_t c = Memory.RAM[a];
        if (b != c) {
            out_addrs[n] = a;
            out_before[n] = b;
            out_after[n]  = c;
            n++;
        }
    }
    return n;
}

void snes9x_bridge_shutdown(void) {
    if (!s_loaded) return;
    retro_unload_game();
    retro_deinit();
    s_rom_bytes.clear();
    s_loaded = false;
}

int snes9x_bridge_is_loaded(void) {
    return s_loaded ? 1 : 0;
}

void snes9x_bridge_get_wram(uint8_t *out) {
    if (!out) return;
    if (!s_loaded) { memset(out, 0, 0x20000); return; }
    /* snes9x's WRAM is Memory.RAM[0x20000] — bank 7E:7F laid out
     * contiguously, identical layout to our runner's snes->ram. */
    memcpy(out, Memory.RAM, 0x20000);
}

void snes9x_bridge_get_vram(uint8_t *out) {
    if (!out) return;
    if (!s_loaded) { memset(out, 0, 0x10000); return; }
    /* snes9x's VRAM is Memory.VRAM[0x10000] — same layout as the
     * SNES PPU's word-addressed VRAM (byte-indexed here). */
    memcpy(out, Memory.VRAM, 0x10000);
}

extern "C" void snes9x_bridge_get_cgram(uint8_t *out) {
    if (!out) return;
    if (!s_loaded) { memset(out, 0, 512); return; }
    /* snes9x stores CGRAM as 256 16-bit BGR15 words in PPU.CGDATA[].
     * Recomp's g_ppu->cgram is a flat byte array (256*2 bytes = 512).
     * Marshal to byte-array layout for direct comparison. */
    for (int i = 0; i < 256; i++) {
        out[i * 2 + 0] = (uint8_t)(PPU.CGDATA[i] & 0xFF);
        out[i * 2 + 1] = (uint8_t)((PPU.CGDATA[i] >> 8) & 0xFF);
    }
}

extern "C" void snes9x_bridge_get_oam(uint8_t *out) {
    if (!out) return;
    if (!s_loaded) { memset(out, 0, 544); return; }
    /* PPU.OAMData = 512 main + 32 high bytes, contiguous. Same shape
     * as recomp's g_ppu->oam (512) + g_ppu->highOam (32). */
    memcpy(out, PPU.OAMData, 544);
}

/* PPU/DMA register shadow — snes9x mirrors the most recent CPU write
 * to each MMIO register in Memory.FillRAM[]. FillRAM[0x2100..0x21FF]
 * covers every PPU register; FillRAM[0x4300..0x437F] covers all 8 DMA
 * channels (16 bytes each). Recomp's runtime keeps an equivalent
 * shadow so byte-by-byte diff is meaningful. */
extern "C" void snes9x_bridge_get_ppu_regs(uint8_t *out, int len) {
    if (!out || len < 1) return;
    if (!s_loaded) { memset(out, 0, len); return; }
    if (len > 0x100) len = 0x100;
    memcpy(out, &Memory.FillRAM[0x2100], len);
}

extern "C" void snes9x_bridge_get_dma_regs(uint8_t *out, int len) {
    if (!out || len < 1) return;
    if (!s_loaded) { memset(out, 0, len); return; }
    if (len > 0x80) len = 0x80;
    memcpy(out, &Memory.FillRAM[0x4300], len);
}

/* ---- Phase B differential fuzz snippet runner --------------------------
 * Write a tiny 65816 snippet to WRAM, seed CPU registers, step opcodes
 * until PC returns to a sentinel value, dump final WRAM. Used by the
 * fuzz harness to produce an oracle-side execution trace for each
 * generated snippet, for byte-level diff against recomp's output.
 *
 * The snippet is placed at bank $00 address $8000 by writing to the
 * raw Memory.RAM (bank $7E) then setting ICPU.ShiftedPB/CPU.PCBase
 * so snes9x reads from the buffer we control. Simpler alternative:
 * write directly into Memory.RAM at an address reachable via PB=$00.
 * Bank $00 $0000-$1FFF is a WRAM mirror, so writing $0000-$1FFF in
 * Memory.RAM and setting PB=$00 PC=$XXXX in that range runs from
 * WRAM. We use bank $00 PC $1800 (unused area below $1F00 state slot).
 *
 * Exit protocol: seed stack with sentinel return address $00DEAC;
 * the snippet's terminating RTS pops PCL/PCH and increments PC by
 * one, giving $00DEAD. The stepper exits when PCw reaches $DEAD.
 */
int snes9x_bridge_fuzz_run_snippet(
    const uint8_t *rom_bytes, int rom_len,
    uint16_t seed_a, uint16_t seed_x, uint16_t seed_y,
    uint16_t seed_s, uint16_t seed_d,
    uint8_t seed_db, uint8_t seed_p,
    uint8_t *out_wram_0_1fff /* caller buffer sized 0x2000 */) {
    if (!s_loaded) return -1;
    if (rom_len <= 0 || rom_len > 0x100) return -2;

    /* Snippet lives at bank $00 / PC $1800 via the bank-$00 WRAM
     * mirror ($0000-$1FFF). Clear the mirror window first so a
     * previous snippet's bytes don't leak into the next one. */
    const uint16_t snippet_pc = 0x1800;
    const uint16_t exit_pc    = 0xDEAD;

    memset(&Memory.RAM[0x0000], 0, 0x2000);
    memcpy(&Memory.RAM[snippet_pc], rom_bytes, rom_len);

    /* Baseline WRAM fixtures the generator expects at $10/$11, $100/$101.
     * The Python runner side uses the same baselines so deltas match. */
    Memory.RAM[0x10]  = 0x55;
    Memory.RAM[0x11]  = 0xAA;
    Memory.RAM[0x100] = 0x33;
    Memory.RAM[0x101] = 0xCC;
    /* Indirect-mode pointer at $20-$22 = bank-$00 ptr to $0100, bank 0. */
    Memory.RAM[0x20] = 0x00; Memory.RAM[0x21] = 0x01; Memory.RAM[0x22] = 0x00;
    /* LONG-mode target $7E0200. */
    Memory.RAM[0x200] = 0x77; Memory.RAM[0x201] = 0x88;
    /* Flag-capture slots $1F06-$1F09 pre-seeded to 0xFF so the
     * conditional-STZ capture pattern in the snippet epilogue
     * produces a distinguishable delta when a flag is set. See
     * generate_snippets.py::epilogue() for the protocol. */
    Memory.RAM[0x1F06] = 0xFF; Memory.RAM[0x1F07] = 0xFF;
    Memory.RAM[0x1F08] = 0xFF; Memory.RAM[0x1F09] = 0xFF;

    /* Seed registers. */
    Registers.A.W   = seed_a;
    Registers.X.W   = seed_x;
    Registers.Y.W   = seed_y;
    Registers.S.W   = seed_s;
    Registers.D.W   = seed_d;
    Registers.DB    = seed_db;
    Registers.PB    = 0x00;
    Registers.PCw   = snippet_pc;
    Registers.P.B.l = seed_p;
    /* Clear emulation-mode bit: snippets run in native mode. */
    Registers.P.W  &= ~256u;

    /* Seed return sentinel onto the stack: RTS pops PCL then PCH
     * and adds 1, so we push (exit_pc - 1) = $DEAC as (hi, lo).
     * In native mode S.W is the full 16-bit stack pointer; pages
     * $00-$01 of bank $00 are WRAM-mirrored, so Memory.RAM[S.W]
     * is the correct indexing. */
    uint16_t ret = (uint16_t)(exit_pc - 1);
    Memory.RAM[Registers.S.W] = (uint8_t)(ret >> 8);
    Registers.S.W--;
    Memory.RAM[Registers.S.W] = (uint8_t)(ret & 0xFF);
    Registers.S.W--;

    S9xUnpackStatus();
    S9xFixCycles();
    ICPU.ShiftedPB = 0;  /* PB=$00 */
    S9xSetPCBase(snippet_pc);

    /* Step opcodes until PC == exit_pc or we hit a safety budget. */
    const int MAX_STEPS = 4096;
    int steps = 0;
    while (Registers.PBPC != ((uint32_t)0x00 << 16 | exit_pc)) {
        if (steps++ >= MAX_STEPS) {
            S9xPackStatus();
            /* Dump WRAM anyway so the caller can observe partial state. */
            if (out_wram_0_1fff) memcpy(out_wram_0_1fff, &Memory.RAM[0x0000], 0x2000);
            /* Encode final PC24 into the negative return so the caller
             * can diagnose without another trip. Negative values > -256
             * stay reserved for clean error codes; PC goes into lower
             * bits of a pattern below -1000. */
            int32_t pc24 = (int32_t)(Registers.PBPC & 0xffffff);
            return -1000 - pc24;
        }
        uint8_t Op;
        if (CPU.PCBase) {
            Op = CPU.PCBase[Registers.PCw];
        } else {
            Op = S9xGetByte(Registers.PBPC);
            OpenBus = Op;
        }
        /* Recheck PCBase after a block-boundary cross. */
        if ((Registers.PCw & MEMMAP_MASK) + ICPU.S9xOpLengths[Op] >= MEMMAP_BLOCK_SIZE) {
            CPU.PCBase = S9xGetBasePointer(ICPU.ShiftedPB + ((uint16)(Registers.PCw + 4)));
        }
        Registers.PCw++;
        (*ICPU.S9xOpcodes[Op].S9xOpcode)();
    }
    S9xPackStatus();

    /* Dump WRAM $0000-$1FFF for the caller. */
    if (out_wram_0_1fff) {
        memcpy(out_wram_0_1fff, &Memory.RAM[0x0000], 0x2000);
    }
    return 0;
}

uint8_t snes9x_bridge_cpu_read(uint32_t addr24) {
    if (!s_loaded) return 0xFF;
    return S9xGetByte(addr24);
}

void snes9x_bridge_get_cpu_regs(SnesCpuRegs *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_loaded) return;
    out->a  = Registers.A.W;
    out->x  = Registers.X.W;
    out->y  = Registers.Y.W;
    out->s  = Registers.S.W;
    out->d  = Registers.D.W;
    out->pc = Registers.PC.W.xPC;
    out->db = Registers.DB;
    out->pb = Registers.PC.B.xPB;
    out->p  = Registers.P.B.l;
    /* Emulation flag: bit 8 of the 16-bit P word (see 65c816.h:
     *   #define Emulation 256
     *   #define CheckEmulation() (Registers.P.W & Emulation) ). */
    out->emulation_mode = (Registers.P.W & 256u) ? 1 : 0;
}

/* The hook pointers cpuexec.cpp / getset.h reference. NULL by default;
 * snes9x_bridge_insn_trace_on installs the trampoline below. */
extern "C" void (*s9x_insn_hook)(uint8_t pb, uint16_t pc, uint8_t op) = nullptr;
extern "C" void (*s9x_nmi_hook)(void) = nullptr;

extern "C" void s9x_insn_hook_trampoline(uint8_t pb, uint16_t pc, uint8_t op) {
    s9x_bridge_insn_hook(pb, pc, op);
}
extern "C" void s9x_nmi_hook_trampoline(void) {
    s9x_bridge_nmi_hook();
}

/* VRAM byte-write hook pointer. ppu.h's S9xVRAMByteWrite chokepoint
 * fires this for every CPU-visible VRAM byte write (REGISTER_2118
 * + REGISTER_2119 + tile/linear variants). Always-on once
 * s9x_vram_hook_arm installs it; the recording side
 * (debug_server.c::debug_server_on_oracle_vram_write) gates on its
 * own active flag so untriggered builds pay just one null-load +
 * branch per VRAM write. */
extern "C" void (*s9x_vram_hook)(uint32_t byte_addr, uint8_t value) = nullptr;

/* Forward decl of the C-side recorder; defined in debug_server.c. */
extern "C" void debug_server_on_oracle_vram_write(uint32_t byte_addr, uint8_t value);

extern "C" void s9x_vram_hook_trampoline(uint32_t byte_addr, uint8_t value) {
    debug_server_on_oracle_vram_write(byte_addr, value);
}

void snes9x_bridge_vram_hook_arm(void) {
    s9x_vram_hook = s9x_vram_hook_trampoline;
}

/* ---- Per-instruction trace public API ---- */

void snes9x_bridge_insn_trace_on(void) {
    s_emu_insn_active = 1;
    s9x_insn_hook = s9x_insn_hook_trampoline;
    s9x_nmi_hook  = s9x_nmi_hook_trampoline;
}

/* Function-boundary snapshot setter. Pass pc24=0 to disable; any
 * non-zero value installs the snapshot at every dispatch where
 * PB:PC matches. Activates the insn-hook trampoline even if the
 * insn trace is off — the snapshot path runs unconditionally
 * before the trace gate. */
void snes9x_bridge_func_snap_set(uint32_t pc24) {
    s_emu_snap_pc24  = pc24;
    s_emu_snap_count = 0;
    s_emu_snap_frame = -1;
    if (pc24 != 0)
        s9x_insn_hook = s9x_insn_hook_trampoline;
    /* If both insn trace and snap are off, leave the hook installed
     * — its overhead is just the early return + s_emu_snap_pc24 == 0
     * check. Cheap. */
}

int  snes9x_bridge_func_snap_count(void) { return s_emu_snap_count; }
int  snes9x_bridge_func_snap_frame(void) { return s_emu_snap_frame; }
/* Per-call lookup. Caller passes the absolute call index (1-based).
 * Returns nullptr if no longer in ring. Slice length is fixed at
 * 8KB; caller knows that, no need to expose. */
extern "C" const emu_snap_entry* snes9x_bridge_func_snap_lookup(int call_idx) {
    return emu_snap_lookup(call_idx);
}
extern "C" int snes9x_bridge_func_snap_slice_len(void) { return EMU_SNAP_SLICE_LEN; }

void snes9x_bridge_insn_trace_off(void) {
    s_emu_insn_active = 0;
    s9x_insn_hook = nullptr;
    /* Keep s9x_nmi_hook armed even when insn trace is off — the NMI
     * counter is cheap and useful as a standalone metric. Caller can
     * separately query/reset via snes9x_bridge_nmi_count. */
}

uint64_t snes9x_bridge_insn_trace_count(void) { return s_emu_insn_count; }
uint64_t snes9x_bridge_nmi_count(void) { return s_emu_nmi_count; }

void snes9x_bridge_insn_trace_reset(void) {
    s_emu_insn_active = 0;
    s_emu_insn_write_idx = 0;
    s_emu_insn_count = 0;
    s9x_insn_hook = nullptr;
}

/* Random-access reader for one entry by relative index (0 = oldest
 * still in ring). Returns 1 on success. */
int snes9x_bridge_insn_trace_get(uint64_t i, int32_t *frame,
                                 uint32_t *pc24, uint8_t *op,
                                 uint8_t *db, uint16_t *a, uint16_t *x,
                                 uint16_t *y, uint16_t *s, uint16_t *d,
                                 uint16_t *p_w, int32_t *cycles) {
    if (i >= s_emu_insn_count || !s_emu_insn_trace || s_emu_insn_capacity == 0)
        return 0;
    uint64_t start = (s_emu_insn_count < s_emu_insn_capacity) ? 0 :
                     (s_emu_insn_write_idx - s_emu_insn_capacity);
    uint64_t idx = (start + i) % s_emu_insn_capacity;
    auto &e = s_emu_insn_trace[idx];
    if (frame)  *frame  = e.frame;
    if (pc24)   *pc24   = e.pc24;
    if (op)     *op     = e.op;
    if (db)     *db     = e.db;
    if (a)      *a      = e.a;
    if (x)      *x      = e.x;
    if (y)      *y      = e.y;
    if (s)      *s      = e.s;
    if (d)      *d      = e.d;
    if (p_w)    *p_w    = e.p_w;
    if (cycles) *cycles = e.cycles;
    return 1;
}

/* ---- Tier-1 WRAM watchpoint public API ---- */

int snes9x_bridge_watch_add(uint32_t lo, uint32_t hi) {
    if (lo > hi || hi >= 0x20000) return -1;
    if (s_watch_nranges >= EMU_WATCH_MAX_RANGES) return -2;
    s_watch_ranges[s_watch_nranges].lo = lo;
    s_watch_ranges[s_watch_nranges].hi = hi;
    s_watch_nranges++;
    s_watch_active = 1;
    /* Installing the hook makes every snes9x write pay one null-check.
     * Reverts to nullptr on clear. */
    s9x_write_hook = s9x_write_hook_trampoline;
    return s_watch_nranges;
}

void snes9x_bridge_watch_clear(void) {
    s_watch_active = 0;
    s_watch_nranges = 0;
    s_watch_write_idx = 0;
    s_watch_count = 0;
    s9x_write_hook = nullptr;
}

int snes9x_bridge_watch_count(void) { return s_watch_count; }

uint32_t snes9x_bridge_get_frame(void) { return s_watch_frame; }

int snes9x_bridge_watch_get(int i, uint32_t *frame, uint32_t *addr,
                            uint32_t *pc24, uint8_t *before, uint8_t *after,
                            uint8_t *bank_source) {
    if (i < 0 || i >= s_watch_count) return 0;
    int start = s_watch_count < EMU_WATCH_LOG_SIZE ? 0 :
                s_watch_write_idx - EMU_WATCH_LOG_SIZE;
    int idx = (start + i) % EMU_WATCH_LOG_SIZE;
    if (frame)       *frame       = s_watch_log[idx].frame;
    if (addr)        *addr        = s_watch_log[idx].addr;
    if (pc24)        *pc24        = s_watch_log[idx].pc24;
    if (before)      *before      = s_watch_log[idx].byte_before;
    if (after)       *after       = s_watch_log[idx].byte_after;
    if (bank_source) *bank_source = (uint8_t)s_watch_log[idx].bank_source;
    return 1;
}

/* ---- Backend instance registered in emu_oracle_cmds.c ---- */
const snes_oracle_backend_t g_snes9x_backend = {
    /* .name          = */ "snes9x",
    /* .init          = */ snes9x_bridge_init,
    /* .run_frame     = */ snes9x_bridge_run_frame,
    /* .shutdown      = */ snes9x_bridge_shutdown,
    /* .is_loaded     = */ snes9x_bridge_is_loaded,
    /* .get_wram      = */ snes9x_bridge_get_wram,
    /* .cpu_read      = */ snes9x_bridge_cpu_read,
    /* .get_cpu_regs  = */ snes9x_bridge_get_cpu_regs,
    /* .get_vram      = */ snes9x_bridge_get_vram,
    /* .get_cgram     = */ snes9x_bridge_get_cgram,
    /* .get_oam       = */ snes9x_bridge_get_oam,
    /* .get_ppu_regs  = */ snes9x_bridge_get_ppu_regs,
    /* .get_dma_regs  = */ snes9x_bridge_get_dma_regs,
};

} /* extern "C" */
