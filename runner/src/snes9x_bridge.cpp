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
#define EMU_WATCH_LOG_SIZE   16384

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
    /* snes9x libretro default is XRGB8888. Copy what fits into 256x240. */
    unsigned copy_h = height > 240 ? 240 : height;
    unsigned copy_w = width > 256 ? 256 : width;
    for (unsigned y = 0; y < copy_h; y++) {
        memcpy(s_framebuf_xrgb + y * 256,
               (const uint8_t *)data + y * pitch,
               copy_w * sizeof(uint32_t));
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
            /* Accept whatever snes9x wants (XRGB8888 by default on x64). */
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
 * Storage: ring of (frame, pc24, op, A, X, Y, S, D, DB, P_W, cycles)
 * = 24 bytes per entry × 1M = 24 MB. Suitable for ~10 seconds of
 * attract demo recording.
 */
#define EMU_INSN_TRACE_SIZE (1u << 20)  /* 1M entries */

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
emu_insn_entry    s_emu_insn_trace[EMU_INSN_TRACE_SIZE];

/* NMI counter — ticks every NMI dispatch. Useful for "how many NMIs
 * fired between block_idx X and Y" cadence comparisons. */
uint64_t s_emu_nmi_count = 0;
int      s_emu_last_nmi_frame = -1;

void s9x_bridge_insn_hook(uint8_t pb, uint16_t pc, uint8_t op) {
    if (!s_emu_insn_active) return;
    uint64_t idx = s_emu_insn_write_idx % EMU_INSN_TRACE_SIZE;
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
    if (s_emu_insn_count < EMU_INSN_TRACE_SIZE) s_emu_insn_count++;
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
    return 0;
}

void snes9x_bridge_run_frame(uint16_t joypad1, uint16_t joypad2) {
    if (!s_loaded) return;
    s_joypad[0] = joypad1;
    s_joypad[1] = joypad2;
    /* Snapshot WRAM so emu_wram_delta can report which bytes the
     * frame's execution changed. Cheap (128 KB memcpy per frame). */
    memcpy(s_wram_before, Memory.RAM, 0x20000);
    s_watch_frame++;
    retro_run();
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

/* ---- Per-instruction trace public API ---- */

void snes9x_bridge_insn_trace_on(void) {
    s_emu_insn_active = 1;
    s9x_insn_hook = s9x_insn_hook_trampoline;
    s9x_nmi_hook  = s9x_nmi_hook_trampoline;
}

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
    if (i >= s_emu_insn_count) return 0;
    uint64_t start = (s_emu_insn_count < EMU_INSN_TRACE_SIZE) ? 0 :
                     (s_emu_insn_write_idx - EMU_INSN_TRACE_SIZE);
    uint64_t idx = (start + i) % EMU_INSN_TRACE_SIZE;
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
};

} /* extern "C" */
