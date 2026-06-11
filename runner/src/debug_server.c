// debug_server.c — Embedded TCP debug server for snesrecomp-v2
// Provides on-demand memory inspection, breakpoints, and frame control.
// Protocol: line-based text commands over TCP (one command per line, \n terminated).
// Responses are JSON-ish single lines followed by \n.
//
// Threading model: a background thread handles TCP accept/recv/send so the server
// stays responsive even when the main game thread is blocked. The main thread
// records frame data via debug_server_record_frame(). A mutex protects shared state
// (frame history, watchpoints, dispatch trace).

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSESOCKET closesocket
#include <process.h>  // _beginthreadex
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
typedef int socket_t;
#define SOCKET_INVALID -1
#define CLOSESOCKET close
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include "debug_server.h"

// External references
extern const char *g_last_recomp_func;
extern int snes_frame_counter;

// Hardware state access (for exhaustive debug dumps)
#include "snes/ppu.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/apu.h"
#include "snes/spc.h"
#include "snes/snes.h"
#include "snes/saveload.h"
#include "cpu_state.h"
#include "cpu_trace.h"
extern Ppu *g_ppu;
extern Cpu *g_snes_cpu;
extern Dma *g_dma;
extern Snes *g_snes;
// APU pacing counter; defined in common_rtl.c.
extern uint64_t g_main_cpu_cycles_estimate;
extern uint8 g_ram[0x20000];
extern uint8 *g_sram;
extern int g_sram_size;
void snes_saveload(Snes *snes, SaveLoadInfo *sli);

// Note: g_snes->ram == g_ram (same pointer, see snes_init). The dual-WRAM
// pattern this file once bridged was phantom — both "sides" always pointed
// to the same 128KB buffer. Single-PPU likewise (see Tier 3d).

#define RECOMP_STACK_DEPTH 16
extern const char *g_recomp_stack[];
extern int g_recomp_stack_top;

// Server state
static socket_t s_listen_sock = SOCKET_INVALID;
static socket_t s_client_sock = SOCKET_INVALID;
static uint8_t *s_ram = NULL;
static uint32_t s_ram_size = 0;
// Note: s_frame_counter pointer removed — use snes_frame_counter directly
static volatile int s_paused = 0;
static volatile int s_step_remaining = 0;  // frames remaining before auto-re-pause
static volatile int s_pending_loadstate = -1;  // -1 = none, 0-9 = slot to load
static volatile uint32_t s_controller_inputs = 0;  // p1 bits 0..11, p2 bits 12..23
static volatile uint32_t s_controller_active = 0;  // bit0=p1 present, bit1=p2 present

// Threading state
#ifdef _WIN32
static CRITICAL_SECTION s_mutex;
static HANDLE s_thread = NULL;
#else
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static int s_thread_created = 0;
#endif
static volatile int s_shutdown = 0;

// Forward declarations for thread function
#ifdef _WIN32
static unsigned __stdcall debug_server_thread(void *arg);
#else
static void *debug_server_thread(void *arg);
#endif

static void lock_mutex(void) {
#ifdef _WIN32
    EnterCriticalSection(&s_mutex);
#else
    pthread_mutex_lock(&s_mutex);
#endif
}

static void unlock_mutex(void) {
#ifdef _WIN32
    LeaveCriticalSection(&s_mutex);
#else
    pthread_mutex_unlock(&s_mutex);
#endif
}

// WRAM write watchpoints
#define MAX_WATCHPOINTS 8
static struct {
    uint32_t addr;
    uint8_t prev_val;
    int active;
} s_watchpoints[MAX_WATCHPOINTS];

// ---- Address write trace ----
// Records every detected value change at a traced address, with call stack.
#define TRACE_LOG_SIZE 256
#define TRACE_STACK_DEPTH 8  // max stack frames captured per entry
static struct {
    uint32_t addr;
    int active;
    uint8_t prev_val;
    int write_idx;
    int count;
    struct {
        int frame;
        uint8_t old_val;
        uint8_t new_val;
        char func[64];
        const char *stack[TRACE_STACK_DEPTH];
        int stack_depth;
    } log[TRACE_LOG_SIZE];
} s_addr_trace = {0};

static void check_addr_trace(void) {
    if (!s_addr_trace.active || !s_ram) return;
    uint8_t cur = s_ram[s_addr_trace.addr];
    if (cur != s_addr_trace.prev_val) {
        extern const char *g_last_recomp_func;
        extern const char *g_recomp_stack[];
        extern int g_recomp_stack_top;
        int idx = s_addr_trace.write_idx % TRACE_LOG_SIZE;
        s_addr_trace.log[idx].frame = snes_frame_counter;
        s_addr_trace.log[idx].old_val = s_addr_trace.prev_val;
        s_addr_trace.log[idx].new_val = cur;
        if (g_last_recomp_func)
            strncpy(s_addr_trace.log[idx].func, g_last_recomp_func, 63);
        else
            strcpy(s_addr_trace.log[idx].func, "(none)");
        s_addr_trace.log[idx].func[63] = 0;
        // Capture call stack snapshot (bottom-up: [0]=deepest caller, last=current)
        int depth = g_recomp_stack_top < TRACE_STACK_DEPTH ? g_recomp_stack_top : TRACE_STACK_DEPTH;
        s_addr_trace.log[idx].stack_depth = depth;
        for (int s = 0; s < depth; s++)
            s_addr_trace.log[idx].stack[s] = g_recomp_stack[g_recomp_stack_top - depth + s];
        s_addr_trace.write_idx++;
        if (s_addr_trace.count < TRACE_LOG_SIZE) s_addr_trace.count++;
        s_addr_trace.prev_val = cur;
    }
}

// ---- Range write trace ----
// Monitors a contiguous byte range and records any per-byte value change
// each poll cycle, tagged with the base offset. Designed for watching
// small arrays (e.g. SpriteBlockedDirs $1588..$1593 = 12 slots).
#define RANGE_TRACE_MAX 16
#define RANGE_TRACE_LOG_SIZE 2048
static struct {
    uint32_t base;
    int len;
    int active;
    uint8_t prev_val[RANGE_TRACE_MAX];
    int write_idx;
    int count;
    struct {
        int frame;
        uint16_t offset;
        uint8_t old_val;
        uint8_t new_val;
        char func[64];
        const char *stack[TRACE_STACK_DEPTH];
        int stack_depth;
    } log[RANGE_TRACE_LOG_SIZE];
} s_range_trace = {0};

static void check_range_trace(void) {
    if (!s_range_trace.active || !s_ram) return;
    extern const char *g_last_recomp_func;
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    for (int i = 0; i < s_range_trace.len; i++) {
        uint8_t cur = s_ram[s_range_trace.base + i];
        if (cur == s_range_trace.prev_val[i]) continue;
        int idx = s_range_trace.write_idx % RANGE_TRACE_LOG_SIZE;
        s_range_trace.log[idx].frame = snes_frame_counter;
        s_range_trace.log[idx].offset = (uint16_t)i;
        s_range_trace.log[idx].old_val = s_range_trace.prev_val[i];
        s_range_trace.log[idx].new_val = cur;
        if (g_last_recomp_func)
            strncpy(s_range_trace.log[idx].func, g_last_recomp_func, 63);
        else
            strcpy(s_range_trace.log[idx].func, "(none)");
        s_range_trace.log[idx].func[63] = 0;
        int depth = g_recomp_stack_top < TRACE_STACK_DEPTH ? g_recomp_stack_top : TRACE_STACK_DEPTH;
        s_range_trace.log[idx].stack_depth = depth;
        for (int s = 0; s < depth; s++)
            s_range_trace.log[idx].stack[s] = g_recomp_stack[g_recomp_stack_top - depth + s];
        s_range_trace.write_idx++;
        if (s_range_trace.count < RANGE_TRACE_LOG_SIZE) s_range_trace.count++;
        s_range_trace.prev_val[i] = cur;
    }
}

// ---- MMIO register-write trace ----
// Captures every write to an MMIO register address in any configured
// [lo, hi] range, tagged with frame + last recomp func + call-stack.
// Enabled via "trace_reg <lo> <hi>" (appends a range, up to
// MAX_TRACE_RANGES); read via "get_reg_trace"; cleared via
// "trace_reg_reset".
#define REG_TRACE_LOG_SIZE 32768
#define MAX_TRACE_RANGES 8
static struct {
    int active;
    int nranges;
    struct { uint16_t lo, hi; } ranges[MAX_TRACE_RANGES];
    int write_idx;
    int count;
    struct {
        int frame;
        uint16_t adr;
        uint8_t val;
        char func[64];
        const char *stack[TRACE_STACK_DEPTH];
        int stack_depth;
    } log[REG_TRACE_LOG_SIZE];
} s_reg_trace = {0};

// ---- VRAM byte-write trace ----
// Captures every CPU-visible byte write to PPU VRAM with full recomp-
// side attribution (frame, function, recomp-call stack). Byte-addressed
// (matching the oracle ring) so cmd_vram_write_diff can walk the two
// rings as parallel byte-pair sequences. Each $2118 STA adds one event
// at byte_addr = (vramPointer << 1) + 0; each $2119 STA adds one event
// at byte_addr = (vramPointer << 1) + 1; WriteVramWord adds both.
//
// Enabled via "trace_vram <lo> <hi>" (BYTE addresses 0..$FFFF) up to
// MAX_VRAM_TRACE_RANGES disjoint ranges; read via "get_vram_trace";
// cleared via "trace_vram_reset". The default-armed range covers all
// of byte-VRAM ($0000-$FFFF), so probes never need to re-arm.
/* HEAP-allocated rings (calloc'd at debug_server_init) instead of
 * static BSS. Sized to ~1.34 GB total resident (recomp ~1.28 GB +
 * oracle ~64 MB) so the always-on capture stays well under 1.5 GB
 * while spanning many minutes of attract-demo. The Oracle build's
 * existing BSS already runs ~1.85 GB (s_frame_history alone is
 * 1.2 GB), and the linker+loader stop accepting PE images near
 * 2 GB; heap-allocation sidesteps that ceiling entirely.
 *
 * Default 8M entries × ~160 bytes ≈ 1.28 GB recomp + 8M × 8 bytes
 * ≈ 64 MB oracle. Coverage estimate: ~16 000 frames at boot peak
 * (~500 writes/frame), ~400 000 frames at attract steady state
 * (~20 writes/frame). Override with SNESRECOMP_VRAM_RING_ENTRIES
 * if a smaller box can't spare the RAM. */
#define VRAM_TRACE_DEFAULT_ENTRIES (8u * 1024u * 1024u)
#define MAX_VRAM_TRACE_RANGES 8

typedef struct {
    int frame;
    uint16_t adr_byte;
    uint8_t  val;
    uint8_t  pad;
    /* CpuState snapshot at write time. Lets the differ surface
     * "what was X when this byte was written" — usually the
     * difference between "bug is somewhere upstream" and "bug is
     * a width-mask in function F at index G". */
    uint16_t A;
    uint16_t X;
    uint16_t Y;
    uint16_t D;
    uint8_t  DB;
    uint8_t  P;
    uint8_t  m_flag;
    uint8_t  x_flag;
    char func[64];
    const char *stack[TRACE_STACK_DEPTH];
    int stack_depth;
} VramTraceEntry;

static struct {
    int active;
    int nranges;
    struct { uint16_t lo, hi; } ranges[MAX_VRAM_TRACE_RANGES];
    uint64_t write_idx;
    uint64_t count;
    uint64_t capacity;          /* set at vram_trace_alloc time */
    VramTraceEntry *log;        /* heap-allocated; calloc'd at init */
} s_vram_trace = {0};

void debug_server_on_vram_write(uint32_t byte_addr, uint8_t value) {
    if (!s_vram_trace.active || !s_vram_trace.log) return;
    uint16_t adr_b = (uint16_t)(byte_addr & 0xFFFF);
    int hit = 0;
    for (int i = 0; i < s_vram_trace.nranges; i++)
        if (adr_b >= s_vram_trace.ranges[i].lo &&
            adr_b <= s_vram_trace.ranges[i].hi) { hit = 1; break; }
    if (!hit) return;
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    uint64_t idx = s_vram_trace.write_idx % s_vram_trace.capacity;
    VramTraceEntry *e = &s_vram_trace.log[idx];
    e->frame = snes_frame_counter;
    e->adr_byte = adr_b;
    e->val = value;
    e->A = g_cpu.A;
    e->X = g_cpu.X;
    e->Y = g_cpu.Y;
    e->D = g_cpu.D;
    e->DB = g_cpu.DB;
    e->P = g_cpu.P;
    e->m_flag = g_cpu.m_flag;
    e->x_flag = g_cpu.x_flag;
    if (g_last_recomp_func)
        strncpy(e->func, g_last_recomp_func, 63);
    else
        strcpy(e->func, "(none)");
    e->func[63] = 0;
    int depth = g_recomp_stack_top < TRACE_STACK_DEPTH ? g_recomp_stack_top : TRACE_STACK_DEPTH;
    e->stack_depth = depth;
    for (int s = 0; s < depth; s++)
        e->stack[s] = g_recomp_stack[g_recomp_stack_top - depth + s];
    s_vram_trace.write_idx++;
    if (s_vram_trace.count < s_vram_trace.capacity) s_vram_trace.count++;
}

// ---- Oracle-side VRAM byte-write trace ----
// Mirrors s_vram_trace above but byte-addressed (snes9x stores bytes,
// not words) and without function/stack attribution: snes9x is the
// reference; we trust its writes are correct. The differ
// (cmd_vram_write_diff) walks both rings forward and reports the first
// mismatched (addr, value) pair restricted to a requested byte-address
// range — that mechanically pinpoints which recomp-side function
// produced the divergent VRAM byte.
//
// Heap-allocated to match the recomp ring; capacity set at init.

typedef struct {
    int      frame;
    uint16_t adr_byte;
    uint8_t  val;
    uint8_t  pad;
} OracleVramTraceEntry;

static struct {
    int active;
    uint64_t write_idx;
    uint64_t count;
    uint64_t capacity;
    OracleVramTraceEntry *log;
} s_oracle_vram_trace = {0};

void debug_server_on_oracle_vram_write(uint32_t byte_addr, uint8_t value) {
    if (!s_oracle_vram_trace.active || !s_oracle_vram_trace.log) return;
    uint64_t idx = s_oracle_vram_trace.write_idx % s_oracle_vram_trace.capacity;
    s_oracle_vram_trace.log[idx].frame = snes_frame_counter;
    s_oracle_vram_trace.log[idx].adr_byte = (uint16_t)(byte_addr & 0xFFFF);
    s_oracle_vram_trace.log[idx].val = value;
    s_oracle_vram_trace.write_idx++;
    if (s_oracle_vram_trace.count < s_oracle_vram_trace.capacity)
        s_oracle_vram_trace.count++;
}

/* Heap-allocate the recomp + oracle VRAM rings. Honors
 * SNESRECOMP_VRAM_RING_ENTRIES env override (decimal entries; clamped
 * to [1<<16, 1<<28] to keep math sane). Returns the chosen capacity
 * so callers can log it. Idempotent — second call frees + reallocs. */
static uint64_t vram_trace_alloc_rings(void) {
    uint64_t cap = VRAM_TRACE_DEFAULT_ENTRIES;
    const char *env = getenv("SNESRECOMP_VRAM_RING_ENTRIES");
    if (env && *env) {
        unsigned long long v = strtoull(env, NULL, 0);
        if (v >= (1ULL << 16) && v <= (1ULL << 28)) cap = (uint64_t)v;
    }
    if (s_vram_trace.log) free(s_vram_trace.log);
    if (s_oracle_vram_trace.log) free(s_oracle_vram_trace.log);
    s_vram_trace.log = (VramTraceEntry *)calloc((size_t)cap, sizeof(VramTraceEntry));
    s_vram_trace.capacity = s_vram_trace.log ? cap : 0;
    s_vram_trace.write_idx = 0;
    s_vram_trace.count = 0;
    s_oracle_vram_trace.log = (OracleVramTraceEntry *)
        calloc((size_t)cap, sizeof(OracleVramTraceEntry));
    s_oracle_vram_trace.capacity = s_oracle_vram_trace.log ? cap : 0;
    s_oracle_vram_trace.write_idx = 0;
    s_oracle_vram_trace.count = 0;
    return cap;
}

/* Expose arm-with-default-ranges so cpu_trace_arm_default_watches can
 * turn on the MMIO ring at process startup, BEFORE the first frame
 * runs. Used 2026-04-30 to capture the boot-time DMA setup that
 * uploads ROM bank $05 bytes to VRAM $7000-$8FFF — that DMA fires
 * around frame 94, before TCP attach can manually arm. */
void debug_server_arm_default_reg_trace(void) {
    s_reg_trace.nranges = 0;
    /* PPU brightness + force-blank + screen-enable.
     * 2026-05-22: added for the MMX first-pass-attract investigation —
     * the screen stays black on first pass with $2100 == $00 (brightness 0),
     * so the writer of the fade-in ramp needs to be observable from frame 0. */
    s_reg_trace.ranges[s_reg_trace.nranges].lo = 0x2100;
    s_reg_trace.ranges[s_reg_trace.nranges].hi = 0x2100; s_reg_trace.nranges++;
    /* PPU VRAM control + CGRAM control */
    s_reg_trace.ranges[s_reg_trace.nranges].lo = 0x2115;
    s_reg_trace.ranges[s_reg_trace.nranges].hi = 0x2119; s_reg_trace.nranges++;
    s_reg_trace.ranges[s_reg_trace.nranges].lo = 0x2121;
    s_reg_trace.ranges[s_reg_trace.nranges].hi = 0x2122; s_reg_trace.nranges++;
    /* DMA channel descriptors */
    s_reg_trace.ranges[s_reg_trace.nranges].lo = 0x4300;
    s_reg_trace.ranges[s_reg_trace.nranges].hi = 0x437F; s_reg_trace.nranges++;
    /* DMA / HDMA enable triggers */
    s_reg_trace.ranges[s_reg_trace.nranges].lo = 0x420B;
    s_reg_trace.ranges[s_reg_trace.nranges].hi = 0x420C; s_reg_trace.nranges++;
    s_reg_trace.active = 1;
}

/* Targeted DMA tripwire — fires the FIRST time a $420B (DMA-trigger)
 * write happens with a channel whose VRAM destination falls in
 * $7000-$8FFF AND whose source bank is $05. Captures rich snapshot.
 *
 * The check decodes the active channels from the FillRAM-equivalent
 * register shadow inside the runtime (we read $4310-$4317 etc. via
 * cpu->ram cache).
 *
 * Investigation 2026-04-30: VRAM $7000-$8FFF in recomp contains raw
 * ROM bytes from $05:CACC..$05:DBCC. The recomp WRAM is byte-clean
 * vs oracle, so the DMA was ROM->VRAM directly. This tripwire pins
 * the writer PC. */
typedef struct {
    uint8_t  armed;
    uint8_t  triggered;
    int      frame;
    uint64_t main_cycles;
    uint64_t trace_idx;
    uint8_t  channel;
    uint8_t  dmap;        /* $43x0 */
    uint8_t  bbus;        /* $43x1 */
    uint16_t a_addr;      /* $43x2/$43x3 */
    uint8_t  a_bank;      /* $43x4 */
    uint16_t size;        /* $43x5/$43x6 */
    uint16_t vram_addr;   /* from $2116/$2117 */
    uint8_t  vmain;       /* $2115 */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag;
    char     last_func[48];
    int      stack_depth;
    char     stack[16][48];
    uint8_t  dma_regs_snap[0x80];   /* full $4300-$437F */
    uint8_t  dp_low_snap[32];       /* $7E:0000-001F */
} DmaTripwire;
static DmaTripwire s_dma_tripwire = {0};

/* Forward decl — defined below in the cmds section. */
static void dma_tripwire_arm_default(void);
/* Public — exposed to cpu_trace.c for boot-time arm. */
void debug_server_arm_default_dma_tripwire(void) { dma_tripwire_arm_default(); }

extern uint8_t g_ram[];
/* Read the RAM shadow of an MMIO register. SMW gen code STAs to
 * $43xx and $21xx with DB=$00 — those addresses are in the LoROM
 * page-0 mirror, NOT in g_ram. The simplest reliable shadow is to
 * keep our own table updated on every cpu_trace_set_reg call. The
 * existing trace_reg ring records writes — re-walk it backward to
 * find the latest value of each register. */
static uint8_t reg_latest_value(uint16_t adr) {
    /* Scan s_reg_trace from the most recent write backward. */
    int n = s_reg_trace.count < REG_TRACE_LOG_SIZE ? s_reg_trace.count : REG_TRACE_LOG_SIZE;
    int start = s_reg_trace.write_idx;
    for (int i = 1; i <= n; i++) {
        int idx = (start - i + REG_TRACE_LOG_SIZE) % REG_TRACE_LOG_SIZE;
        if (s_reg_trace.log[idx].adr == adr) return s_reg_trace.log[idx].val;
    }
    return 0;
}

/* g_cpu_trace_idx is declared (non-volatile) in cpu_trace.h; do not
 * redeclare here. snes_frame_counter and g_main_cpu_cycles_estimate
 * have other extern declarations earlier in this file. */

static void dma_tripwire_check(uint8_t mdmaen);
static void dma_tripwire_arm_default(void) {
    memset(&s_dma_tripwire, 0, sizeof(s_dma_tripwire));
    s_dma_tripwire.armed = 1;
}

/* Called from debug_server_on_reg_write when adr==$420B. Walks each
 * enabled channel in mdmaen and snapshots the FIRST one matching the
 * (vram_dst in $7000-$8FFF) AND (a_bank == $05) criteria. */
static void dma_tripwire_check(uint8_t mdmaen) {
    if (!s_dma_tripwire.armed || s_dma_tripwire.triggered) return;
    uint16_t vram_addr_word = (uint16_t)reg_latest_value(0x2116) |
                              ((uint16_t)reg_latest_value(0x2117) << 8);
    uint16_t vram_addr_byte = (uint16_t)(vram_addr_word << 1);
    /* DMA target VRAM is bbus $18 or $19 ($2118/$2119). */
    for (int ch = 0; ch < 8; ch++) {
        if (!(mdmaen & (1u << ch))) continue;
        uint16_t base = (uint16_t)(0x4300 + (ch << 4));
        uint8_t bbus  = reg_latest_value((uint16_t)(base + 1));
        uint8_t a_bk  = reg_latest_value((uint16_t)(base + 4));
        if (bbus != 0x18 && bbus != 0x19) continue;
        /* VRAM dst in [$7000, $8FFF]? */
        if (vram_addr_byte < 0x7000 || vram_addr_byte > 0x8FFF) continue;
        if (a_bk != 0x05) continue;
        /* MATCH — snapshot. */
        s_dma_tripwire.triggered = 1;
        s_dma_tripwire.frame = snes_frame_counter;
        s_dma_tripwire.main_cycles = g_main_cpu_cycles_estimate;
        s_dma_tripwire.trace_idx = g_cpu_trace_idx;
        s_dma_tripwire.channel = (uint8_t)ch;
        s_dma_tripwire.dmap   = reg_latest_value((uint16_t)(base + 0));
        s_dma_tripwire.bbus   = bbus;
        s_dma_tripwire.a_addr = (uint16_t)reg_latest_value((uint16_t)(base + 2)) |
                                ((uint16_t)reg_latest_value((uint16_t)(base + 3)) << 8);
        s_dma_tripwire.a_bank = a_bk;
        s_dma_tripwire.size   = (uint16_t)reg_latest_value((uint16_t)(base + 5)) |
                                ((uint16_t)reg_latest_value((uint16_t)(base + 6)) << 8);
        s_dma_tripwire.vram_addr = vram_addr_byte;
        s_dma_tripwire.vmain     = reg_latest_value(0x2115);
        s_dma_tripwire.A = g_cpu.A; s_dma_tripwire.X = g_cpu.X; s_dma_tripwire.Y = g_cpu.Y;
        s_dma_tripwire.S = g_cpu.S; s_dma_tripwire.D = g_cpu.D;
        s_dma_tripwire.DB = g_cpu.DB; s_dma_tripwire.PB = g_cpu.PB; s_dma_tripwire.P = g_cpu.P;
        s_dma_tripwire.m_flag = g_cpu.m_flag; s_dma_tripwire.x_flag = g_cpu.x_flag;
        if (g_last_recomp_func) {
            strncpy(s_dma_tripwire.last_func, g_last_recomp_func, 47);
            s_dma_tripwire.last_func[47] = 0;
        }
        int depth = g_recomp_stack_top;
        if (depth > 16) depth = 16;
        s_dma_tripwire.stack_depth = depth;
        int skip = g_recomp_stack_top - depth;
        for (int i = 0; i < depth; i++) {
            const char *p = g_recomp_stack[skip + i];
            if (p) { strncpy(s_dma_tripwire.stack[i], p, 47);
                     s_dma_tripwire.stack[i][47] = 0; }
        }
        /* Snapshot all DMA register shadows so the client can see all
         * 8 channels' setup, not just the one that matched. */
        for (int i = 0; i < 0x80; i++)
            s_dma_tripwire.dma_regs_snap[i] = reg_latest_value((uint16_t)(0x4300 + i));
        memcpy(s_dma_tripwire.dp_low_snap, &g_ram[0x0000], 32);
        return;
    }
}

void debug_server_on_reg_write(uint16_t adr, uint8_t val) {
    /* Hot path: ALWAYS feed the DMA tripwire on $420B writes (DMA
     * trigger), regardless of whether the trace ring is active. The
     * tripwire is one-shot; this avoids missing the boot-time setup
     * because the ring wasn't yet armed. */
    if (adr == 0x420B && val != 0) dma_tripwire_check(val);
    if (!s_reg_trace.active) return;
    int hit = 0;
    for (int i = 0; i < s_reg_trace.nranges; i++)
        if (adr >= s_reg_trace.ranges[i].lo && adr <= s_reg_trace.ranges[i].hi) { hit = 1; break; }
    if (!hit) return;
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    int idx = s_reg_trace.write_idx % REG_TRACE_LOG_SIZE;
    s_reg_trace.log[idx].frame = snes_frame_counter;
    s_reg_trace.log[idx].adr = adr;
    s_reg_trace.log[idx].val = val;
    if (g_last_recomp_func)
        strncpy(s_reg_trace.log[idx].func, g_last_recomp_func, 63);
    else
        strcpy(s_reg_trace.log[idx].func, "(none)");
    s_reg_trace.log[idx].func[63] = 0;
    int depth = g_recomp_stack_top < TRACE_STACK_DEPTH ? g_recomp_stack_top : TRACE_STACK_DEPTH;
    s_reg_trace.log[idx].stack_depth = depth;
    for (int s = 0; s < depth; s++)
        s_reg_trace.log[idx].stack[s] = g_recomp_stack[g_recomp_stack_top - depth + s];
    s_reg_trace.write_idx++;
    if (s_reg_trace.count < REG_TRACE_LOG_SIZE) s_reg_trace.count++;
}

#if SNESRECOMP_REVERSE_DEBUG
void debug_on_recomp_stack_push(const char *name);  // forward (defined below alongside the WRAM trace)
#endif

#if SNESRECOMP_REVERSE_DEBUG
// ---- Tier-1 reverse debugger WRAM write trace ----
// Called synchronously from every WRAM store in the generated C when the
// generator was invoked with --reverse-debug. Filters by up to
// MAX_WRAM_TRACE_RANGES configurable address ranges so we don't log the
// whole world every store. Ring holds ~1M entries, dumped on demand
// via `get_wram_trace`.
#define WRAM_TRACE_LOG_SIZE  (1 << 20)
#define MAX_WRAM_TRACE_RANGES 8
// Addresses are 17-bit (128KB WRAM = $00000..$1FFFF). uint32_t on the
// hot path, uint32_t in the ring, so bank-$7F writes aren't silently
// truncated into bank $7E.
static struct {
    int active;
    int nranges;
    struct { uint32_t lo, hi; } ranges[MAX_WRAM_TRACE_RANGES];
    int write_idx;
    int count;
    struct {
        int frame;
        uint32_t adr;
        uint16_t old_val; // value in WRAM BEFORE the store (added 2026-04-23)
        uint16_t val;     // value after the store (16-bit to hold word writes)
        uint8_t width;    // 1 = byte, 2 = word
        uint64_t block_idx;  // Tier 3: monotonic block counter at time of write
        char func[48];
        char parent[48];  // caller of `func` (one level up the recomp stack)
    } log[WRAM_TRACE_LOG_SIZE];
} s_wram_trace = {0};

/* Arm the dedicated 1M-entry WRAM-write ring at boot for the
 * DP queue/state pointers that have been investigation targets.
 * This ring is SEPARATE from the main cpu_trace ring — it only
 * fills on WRAM writes in the armed ranges, so it survives
 * arbitrarily long BCS-self-loop block storms in the cpu_trace
 * ring. Queryable via `get_wram_trace` from TCP. */
void debug_server_arm_default_wram_trace(void) {
    /* DMA queue tail pointers + neighbors. $7E:00A2 = $DB:0500
     * stride-8 queue tail, $A5/$A6 = $7E:F000 variable-size queue
     * tail, $A3/$A4 historical investigation targets. */
    s_wram_trace.ranges[s_wram_trace.nranges].lo = 0x000A2;
    s_wram_trace.ranges[s_wram_trace.nranges].hi = 0x000A6;
    s_wram_trace.nranges++;
    /* ALttP NMI DMA source tile buffer — decompressed tile data that gets
     * DMA'd to VRAM. Without this range, DMA writes via $2180 (snes_writeBBus
     * case 0x80) are never captured in the ring, making $7E:C700 corruption
     * invisible to wram_writes_at queries. */
    s_wram_trace.ranges[s_wram_trace.nranges].lo = 0x0C700;
    s_wram_trace.ranges[s_wram_trace.nranges].hi = 0x0E6FF;
    s_wram_trace.nranges++;
    s_wram_trace.active = 1;
}

// ---- Tier 3 monotonic block counter ----
// Incremented inside debug_on_block_enter on every basic-block boundary.
// Both the WRAM trace ring (Tier 1) and the block trace ring (Tier 2)
// stamp every entry with the current value so probes can correlate
// "this WRAM write happened during this block." Read-only from outside
// for query purposes.
volatile uint64_t g_block_counter = 0;

// ---- Tier 3 WRAM anchors ----
// Periodic full-WRAM snapshots that let wram_at_block reconstruct
// historical state without replaying millions of writes from boot.
// Disabled by default (zero overhead). When armed via
// tier3_anchor_on, every Nth block_counter value triggers a 128KB
// memcpy snapshot. Ring size cap: ANCHOR_RING_SIZE.
//
// Memory: 64 anchors x 128KB = 8 MB. Covers 64 * anchor_interval
// blocks of replay range; older anchors are evicted FIFO.
#define ANCHOR_RING_SIZE 64
static volatile int     s_anchor_active = 0;
static uint32_t         s_anchor_interval = 4096;  // blocks per anchor
static int              s_anchor_write_idx = 0;
static int              s_anchor_count = 0;
static struct {
    uint64_t block_idx;
    int      frame;
    uint8_t  wram[0x20000];
} s_wram_anchors[ANCHOR_RING_SIZE];

extern uint8_t g_ram[];

// Capture a full WRAM snapshot tagged with the current block_counter.
// Called from debug_on_block_enter when armed and at interval boundaries.
static void anchor_capture(int frame) {
    int idx = s_anchor_write_idx % ANCHOR_RING_SIZE;
    s_wram_anchors[idx].block_idx = g_block_counter;
    s_wram_anchors[idx].frame = frame;
    memcpy(s_wram_anchors[idx].wram, g_ram, 0x20000);
    s_anchor_write_idx++;
    if (s_anchor_count < ANCHOR_RING_SIZE) s_anchor_count++;
}

// Filter hits if ANY byte in [adr, adr+width-1] lies inside any watched range.
static inline int rdb_range_hit(uint32_t adr, uint8_t width) {
    uint32_t lo = adr;
    uint32_t hi = adr + width - 1;
    for (int i = 0; i < s_wram_trace.nranges; i++) {
        uint32_t r_lo = s_wram_trace.ranges[i].lo;
        uint32_t r_hi = s_wram_trace.ranges[i].hi;
        if (lo <= r_hi && hi >= r_lo) return 1;
    }
    return 0;
}

static inline void rdb_record(uint32_t adr, uint16_t old_val, uint16_t new_val, uint8_t width) {
    /* g_boundary_frozen is intentionally NOT checked here. It freezes the NLR
     * boundary-audit ring to preserve a snapshot, but the WRAM write trace ring
     * (s_wram_trace) has its own `active` flag as its sole on/off control. An NLR
     * ancestor-skip should not permanently block WRAM write capture. */
    if (!s_wram_trace.active) return;
    if (!rdb_range_hit(adr, width)) return;
    int idx = s_wram_trace.write_idx % WRAM_TRACE_LOG_SIZE;
    s_wram_trace.log[idx].frame = snes_frame_counter;
    s_wram_trace.log[idx].adr = adr;
    s_wram_trace.log[idx].old_val = old_val;
    s_wram_trace.log[idx].val = new_val;
    s_wram_trace.log[idx].width = width;
    s_wram_trace.log[idx].block_idx = g_block_counter;
    if (g_last_recomp_func)
        strncpy(s_wram_trace.log[idx].func, g_last_recomp_func, 47);
    else
        s_wram_trace.log[idx].func[0] = 0;
    s_wram_trace.log[idx].func[47] = 0;
    // Parent: one level up the recomp stack. g_recomp_stack_top points at
    // the next-free slot, so [top-1] is the current function (same as
    // g_last_recomp_func) and [top-2] is its caller.
    s_wram_trace.log[idx].parent[0] = 0;
    if (g_recomp_stack_top >= 2) {
        const char *p = g_recomp_stack[g_recomp_stack_top - 2];
        if (p) {
            strncpy(s_wram_trace.log[idx].parent, p, 47);
            s_wram_trace.log[idx].parent[47] = 0;
        }
    }
    s_wram_trace.write_idx++;
    if (s_wram_trace.count < WRAM_TRACE_LOG_SIZE) s_wram_trace.count++;
}

// Forward decl — watchpoint state + body live below the Tier 2.5 block.
static void rdb_check_watch(uint32_t addr, uint16_t val, uint8_t width);

void debug_on_wram_write_byte(uint32_t addr, uint8_t old_val, uint8_t val) {
    rdb_record(addr, old_val, val, 1);
    rdb_check_watch(addr, val, 1);
}
void debug_on_wram_write_word(uint32_t addr, uint16_t old_val, uint16_t val) {
    rdb_record(addr, old_val, val, 2);
    rdb_check_watch(addr, val, 2);
}

// ---- Call-trace ring (per-RecompStackPush log) ----
// Active when SNESRECOMP_REVERSE_DEBUG is enabled and trace_calls
// has been turned on. Each entry records (frame, depth, func, parent)
// at the moment of the push. Used to reconstruct the actual call
// sequence within a frame for divergence analysis against SMWDisX.
#define CALL_TRACE_LOG_SIZE 1048576
extern const char *g_recomp_stack[];
extern int g_recomp_stack_top;
static struct {
    int active;
    int write_idx;
    int count;
    struct {
        int frame;
        int depth;
        char func[48];
        char parent[48];
    } log[CALL_TRACE_LOG_SIZE];
} s_call_trace = { .active = 1 };

// ---- Tier-2 block-level execution trace ----
// Emitted at every basic-block boundary (function entry + every label) by
// recomp.py --reverse-debug. Active only when trace_blocks command was
// issued. Used to reconstruct the exact intra-function execution path
// for divergence analysis at sub-function granularity.
//
// Each entry now also captures the recomp's tracked A/X/Y at the
// block-entry moment (RDB_REG_UNKNOWN = 0xFFFFFFFF when the generator
// could not statically pin the register). Closes the gap that the
// emu side has full PC-attributed write tracing while recomp had only
// post-frame snapshots.
#define BLOCK_TRACE_LOG_SIZE 262144
static struct {
    int active;
    int write_idx;
    int count;
    struct {
        int frame;
        int depth;
        uint32_t pc;     // 24-bit bank:pc of the block start
        uint32_t a;      // recomp's tracked A at this block (RDB_REG_UNKNOWN if not pinned)
        uint32_t x;      // tracked X
        uint32_t y;      // tracked Y
        uint64_t block_idx;  // Tier 3: monotonic block counter (this entry's index)
        char func[48];
    } log[BLOCK_TRACE_LOG_SIZE];
} s_block_trace = {0};

// ====================================================================
// Focused OAM-overflow observability (task #7). Two always-on,
// tripwire-frozen, PC-range-filtered traces used to pin the exact
// RTS/return-resolution at the shared $D5CC OAM-finalize tail and the
// block path around it. Both honor g_boundary_frozen (the [oamdrop]
// detector trips it), so the window at the collapse survives while the
// game keeps running. Range set via oamblk_range / rtstrace_range.
// ====================================================================
#define OAMBLK_TRACE_SIZE (1 << 16)
static struct {
    int active; unsigned write_idx; unsigned count;
    struct {
        int frame, depth;
        uint32_t pc;
        uint16_t a, x, y, s, d;
        uint8_t  db, pb, p, mx;
        uint8_t  e3, e4, e6, e8, ea, eb;
        char func[40];
        char parent[40];
    } log[OAMBLK_TRACE_SIZE];
} s_oamblk = {0};
static uint32_t g_oamblk_lo = 1, g_oamblk_hi = 0;  /* lo>hi => off */

#define RTS_TRACE_SIZE (1 << 15)
/* decision codes */
#define RTSDEC_HOST_RETURN   0   /* hrv && ret_s==entry_s -> RECOMP_RETURN_NORMAL  */
#define RTSDEC_DISPATCH      1   /* popped PC is a known dispatch entry -> fp(cpu) */
#define RTSDEC_MISS_UNWIND   2   /* popped PC not an entry -> restore S, NORMAL    */
#define RTSDEC_ANCESTOR_SKIP 3   /* return-to-ancestor: SKIP_N to entry_s==ret_s   */
static struct {
    int active; unsigned write_idx; unsigned count;
    struct {
        int frame;
        uint32_t src_pc;     /* the RTS/RTL instruction PC (block pc)            */
        uint32_t popped_pc;  /* (PB:PC+1) popped return target                   */
        uint16_t entry_s;    /* enclosing host fn's entry S                      */
        uint16_t ret_s;      /* S before the pop                                 */
        uint16_t s_after;    /* S after the pop                                  */
        uint8_t  hrv;        /* host_return_valid at this RTS                    */
        uint8_t  s_eq;       /* ret_s == entry_s                                 */
        uint8_t  decision;   /* RTSDEC_*                                         */
        uint8_t  found;      /* popped_pc is a known dispatch-table entry        */
        uint8_t  e3, e4;     /* OAM park-boundary counters at the RTS            */
        char func[40];       /* g_last_recomp_func (encodes entry PC)            */
    } log[RTS_TRACE_SIZE];
} s_rtst = {0};
static uint32_t g_rtst_lo = 1, g_rtst_hi = 0;  /* lo>hi => off */

// Block-path recorder. Called from cpu_trace_block (has full cpu).
void dbg_oam_block_trace(CpuState *cpu, uint32_t pc24) {
    extern uint8_t g_boundary_frozen;
    if (g_boundary_frozen) return;
    if (pc24 < g_oamblk_lo || pc24 > g_oamblk_hi) return;
    unsigned i = s_oamblk.write_idx % OAMBLK_TRACE_SIZE;
    s_oamblk.log[i].frame = snes_frame_counter;
    s_oamblk.log[i].depth = g_recomp_stack_top;
    s_oamblk.log[i].pc = pc24;
    s_oamblk.log[i].a = (uint16_t)cpu->A;
    s_oamblk.log[i].x = (uint16_t)cpu->X;
    s_oamblk.log[i].y = (uint16_t)cpu->Y;
    s_oamblk.log[i].s = (uint16_t)cpu->S;
    s_oamblk.log[i].d = (uint16_t)cpu->D;
    s_oamblk.log[i].db = cpu->DB;
    s_oamblk.log[i].pb = cpu->PB;
    s_oamblk.log[i].p = cpu->P;
    s_oamblk.log[i].mx = (uint8_t)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
    s_oamblk.log[i].e3 = g_ram[0x0E3]; s_oamblk.log[i].e4 = g_ram[0x0E4];
    s_oamblk.log[i].e6 = g_ram[0x0E6]; s_oamblk.log[i].e8 = g_ram[0x0E8];
    s_oamblk.log[i].ea = g_ram[0x0EA]; s_oamblk.log[i].eb = g_ram[0x0EB];
    if (g_last_recomp_func) { strncpy(s_oamblk.log[i].func, g_last_recomp_func, 39); s_oamblk.log[i].func[39] = 0; }
    else s_oamblk.log[i].func[0] = 0;
    s_oamblk.log[i].parent[0] = 0;
    if (g_recomp_stack_top >= 2) {
        const char *p = g_recomp_stack[g_recomp_stack_top - 2];
        if (p) { strncpy(s_oamblk.log[i].parent, p, 39); s_oamblk.log[i].parent[39] = 0; }
    }
    s_oamblk.write_idx++;
    if (s_oamblk.count < OAMBLK_TRACE_SIZE) s_oamblk.count++;
}

// RTS/return-decision recorder. Called from the emitted RTS/RTL lowering
// AFTER the return frame is popped (so cpu->S == S-after-pop). The
// host-return-vs-dispatch decision is fully determined by (hrv, ret_s,
// entry_s) and a dispatch-table lookup of the popped PC, so we classify
// it here exactly as cpu_dispatch_pc_from / _emit_return would.
void dbg_rts_trace(CpuState *cpu, uint32_t src_pc, uint16_t entry_s,
                   uint16_t ret_s, uint32_t popped_pc, uint8_t hrv) {
    extern uint8_t g_boundary_frozen;
    if (g_boundary_frozen) return;
    if (src_pc < g_rtst_lo || src_pc > g_rtst_hi) return;
    extern int cpu_dispatch_has_entry(CpuState *, uint32_t);
    extern int cpu_resolve_ancestor_skip(uint16_t);
    int found = cpu_dispatch_has_entry(cpu, popped_pc);
    int s_eq = (ret_s == entry_s);
    int decision;
    if (hrv && s_eq)        decision = RTSDEC_HOST_RETURN;
    else if (found)         decision = RTSDEC_DISPATCH;
    else if (!s_eq && cpu_resolve_ancestor_skip(ret_s) >= 0)
                            decision = RTSDEC_ANCESTOR_SKIP;
    else                    decision = RTSDEC_MISS_UNWIND;
    unsigned i = s_rtst.write_idx % RTS_TRACE_SIZE;
    s_rtst.log[i].frame = snes_frame_counter;
    s_rtst.log[i].src_pc = src_pc;
    s_rtst.log[i].popped_pc = popped_pc;
    s_rtst.log[i].entry_s = entry_s;
    s_rtst.log[i].ret_s = ret_s;
    s_rtst.log[i].s_after = (uint16_t)cpu->S;
    s_rtst.log[i].hrv = hrv;
    s_rtst.log[i].s_eq = (uint8_t)s_eq;
    s_rtst.log[i].decision = (uint8_t)decision;
    s_rtst.log[i].found = (uint8_t)found;
    s_rtst.log[i].e3 = g_ram[0x0E3];
    s_rtst.log[i].e4 = g_ram[0x0E4];
    if (g_last_recomp_func) { strncpy(s_rtst.log[i].func, g_last_recomp_func, 39); s_rtst.log[i].func[39] = 0; }
    else s_rtst.log[i].func[0] = 0;
    s_rtst.write_idx++;
    if (s_rtst.count < RTS_TRACE_SIZE) s_rtst.count++;
    /* Validation tripwire (task #7 fix): freeze all rings the instant the
     * return-to-ancestor fix path fires, so a heavy fish-explosion overflow
     * is captured at the exact ANCESTOR_SKIP event (whether or not it would
     * have wiped). This is the decisive proof the overflow now unwinds
     * correctly instead of running the 128-slot park. */
    if (decision == RTSDEC_ANCESTOR_SKIP) {
        extern uint8_t g_boundary_frozen;
        g_boundary_frozen = 1;
    }
}

// ---- Tier 2.5: pause-on-block (breakpoints + single-stepping) ----
// Mirrors the Sonic-recomp design (rdb_on_block_slow / step / continue),
// adapted to our threaded TCP server. The hot path reads two volatile
// ints; almost always falls through. The slow path checks the
// break/step state and uses the EXISTING s_paused mechanism +
// debug_server_wait_if_paused() to park the main thread.
//
// NOTE: pause happens AFTER block-trace recording, so traces still
// capture the parked block.
#define RDB_MAX_BREAKS 16
static volatile int s_rdb_break_armed = 0;        // any of break_pcs are set
static uint32_t s_rdb_break_pcs[RDB_MAX_BREAKS] = {0};
static int s_rdb_break_count = 0;
static volatile int s_rdb_step_pending = 0;       // pause at the next block
static volatile uint32_t s_rdb_parked_pc = 0;     // pc parked at, 0 when not parked

// ---- Tier 2.5: WRAM watchpoints (pause on write) ----
// Runs inside debug_on_wram_write_byte / debug_on_wram_write_word after
// the trace record. Disarmed by default: one volatile-int read + branch
// in the hot path. When a watched store fires, hijacks the same s_paused
// machinery the block breakpoint uses.
//
// Exact-address match only: a byte write at addr matches watches at addr.
// A word write at addr matches watches at addr (low byte) AND at addr+1
// (high byte). The `parked` command reports which watch hit, the value
// that was written, the actual byte stride of the write, and the writing
// function (captured from g_last_recomp_func at the moment of the store).
#define RDB_MAX_WATCHES 16
static volatile int s_rdb_watch_armed = 0;
static struct {
    uint32_t addr;         // WRAM offset (20-bit; bank $7E/$7F safe)
    int32_t  match_val;    // -1 = any value, else compare low bits
} s_rdb_watches[RDB_MAX_WATCHES];
static int s_rdb_watch_count = 0;
// Parked state — observable via `parked` command.
#define RDB_PARKED_STACK_DEPTH 16
static volatile int      s_rdb_parked_watch_idx = -1; // -1 when not parked on watch
static volatile uint32_t s_rdb_parked_watch_addr = 0;
static volatile uint32_t s_rdb_parked_watch_val  = 0;
static volatile uint8_t  s_rdb_parked_watch_width = 0;
static char              s_rdb_parked_watch_func[48];
// Full recomp stack snapshot taken at watch-hit (the single-name
// `writer` above is g_last_recomp_func, which can be stale when a
// callee doesn't set it; the full stack is what tells you the real
// caller chain). Depths beyond RDB_PARKED_STACK_DEPTH are truncated;
// s_rdb_parked_stack_depth reports the true depth so the client can
// tell whether truncation happened.
static char s_rdb_parked_stack[RDB_PARKED_STACK_DEPTH][48];
static volatile int s_rdb_parked_stack_depth = 0;

// Called from debug_on_wram_write_byte/word. Fast path: one volatile read.
// Slow path: linear scan over at most RDB_MAX_WATCHES entries, parks main
// thread via s_paused and debug_server_wait_if_paused if any match.
static void rdb_check_watch(uint32_t addr, uint16_t val, uint8_t width) {
    if (!s_rdb_watch_armed) return;
    int hit_idx = -1;
    uint32_t hit_addr = 0;
    uint32_t hit_val = 0;
    for (int i = 0; i < s_rdb_watch_count; i++) {
        uint32_t wa = s_rdb_watches[i].addr;
        int32_t  mv = s_rdb_watches[i].match_val;
        // Byte write at addr covers [addr,addr]. Word write at addr covers [addr,addr+1].
        if (wa == addr) {
            uint32_t v = (width == 2) ? (val & 0xFFFFu) : (val & 0xFFu);
            if (mv < 0 || (uint32_t)mv == v) {
                hit_idx = i; hit_addr = addr; hit_val = v; break;
            }
        } else if (width == 2 && wa == addr + 1) {
            uint32_t v = (val >> 8) & 0xFFu;
            if (mv < 0 || (uint32_t)mv == v) {
                hit_idx = i; hit_addr = wa; hit_val = v; break;
            }
        }
    }
    if (hit_idx < 0) return;
    s_rdb_parked_watch_idx = hit_idx;
    s_rdb_parked_watch_addr = hit_addr;
    s_rdb_parked_watch_val = hit_val;
    s_rdb_parked_watch_width = width;
    if (g_last_recomp_func) {
        strncpy(s_rdb_parked_watch_func, g_last_recomp_func, 47);
        s_rdb_parked_watch_func[47] = 0;
    } else {
        s_rdb_parked_watch_func[0] = 0;
    }
    // Snapshot the full recomp stack so the client can resolve the
    // actual caller chain even when g_last_recomp_func is stale.
    {
        int top = g_recomp_stack_top;
        int want = top < RDB_PARKED_STACK_DEPTH ? top : RDB_PARKED_STACK_DEPTH;
        int skip = top - want;
        for (int s = 0; s < want; s++) {
            const char *p = g_recomp_stack[skip + s];
            if (p) {
                strncpy(s_rdb_parked_stack[s], p, 47);
                s_rdb_parked_stack[s][47] = 0;
            } else {
                s_rdb_parked_stack[s][0] = 0;
            }
        }
        s_rdb_parked_stack_depth = top;
    }
    s_paused = 1;
    debug_server_wait_if_paused();
    s_rdb_parked_watch_idx = -1;
    s_rdb_parked_stack_depth = 0;
}

void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {
    // Tier 3: bump the monotonic block counter on EVERY hook call,
    // regardless of trace state, so WRAM writes can correlate to block
    // index even when the block trace itself isn't being recorded.
    g_block_counter++;

    // Main-CPU cycle estimate: ~24 cycles per basic block (typically
    // 3-5 65816 instructions averaging ~6 cycles each). Used for trace
    // timestamps and diagnostics. APU catch-up pacing deliberately does
    // NOT read this counter any more — it paces from APU-port touches
    // only (g_apu_pace_cycles_estimate, see common_rtl.c, issue #4).
    g_main_cpu_cycles_estimate += 24;

    // Tier 3 WRAM anchors: snapshot full WRAM every N blocks when armed.
    if (s_anchor_active && (g_block_counter % s_anchor_interval) == 0) {
        anchor_capture(snes_frame_counter);
    }
    if (s_block_trace.active) {
        int idx = s_block_trace.write_idx % BLOCK_TRACE_LOG_SIZE;
        s_block_trace.log[idx].frame = snes_frame_counter;
        s_block_trace.log[idx].depth = g_recomp_stack_top;
        s_block_trace.log[idx].pc = pc;
        s_block_trace.log[idx].a = a;
        s_block_trace.log[idx].x = x;
        s_block_trace.log[idx].y = y;
        s_block_trace.log[idx].block_idx = g_block_counter;
        if (g_last_recomp_func) {
            strncpy(s_block_trace.log[idx].func, g_last_recomp_func, 47);
            s_block_trace.log[idx].func[47] = 0;
        } else {
            s_block_trace.log[idx].func[0] = 0;
        }
        s_block_trace.write_idx++;
        if (s_block_trace.count < BLOCK_TRACE_LOG_SIZE) s_block_trace.count++;
    }
    // Hot-path pause check. Almost always not taken.
    // Plain `pause` cmd via cmd_pause sets s_paused=1; honor it here so
    // the main thread parks at the next block boundary. Without this,
    // setting s_paused=1 from TCP had no effect (the wait was only
    // entered on breakpoint hit). 2026-05-03.
    if (s_paused) {
        debug_server_wait_if_paused();
        return;
    }
    if (!s_rdb_break_armed && !s_rdb_step_pending) return;
    int hit = s_rdb_step_pending;
    if (!hit) {
        for (int i = 0; i < s_rdb_break_count; i++) {
            if (s_rdb_break_pcs[i] == pc) { hit = 1; break; }
        }
    }
    if (hit) {
        s_rdb_step_pending = 0;     // single-step is one-shot
        s_rdb_parked_pc = pc;
        s_paused = 1;
        // Reuse the existing pause-spin loop. TCP thread will clear
        // s_paused via continue / step / step_block / break_continue.
        debug_server_wait_if_paused();
        s_rdb_parked_pc = 0;
    }
}

void debug_on_recomp_stack_push(const char *name) {
    if (!s_call_trace.active) return;
    int idx = s_call_trace.write_idx % CALL_TRACE_LOG_SIZE;
    s_call_trace.log[idx].frame = snes_frame_counter;
    s_call_trace.log[idx].depth = g_recomp_stack_top;
    if (name) {
        strncpy(s_call_trace.log[idx].func, name, 47);
        s_call_trace.log[idx].func[47] = 0;
    } else s_call_trace.log[idx].func[0] = 0;
    // Parent: stack[top-2] (since top-1 is THIS push). RecompStackPush
    // does ++top before calling us, so top-1 IS us — we want top-2.
    s_call_trace.log[idx].parent[0] = 0;
    if (g_recomp_stack_top >= 2) {
        const char *p = g_recomp_stack[g_recomp_stack_top - 2];
        if (p) {
            strncpy(s_call_trace.log[idx].parent, p, 47);
            s_call_trace.log[idx].parent[47] = 0;
        }
    }
    s_call_trace.write_idx++;
    if (s_call_trace.count < CALL_TRACE_LOG_SIZE) s_call_trace.count++;
}
#endif

#include <time.h>
// ---- Per-frame function call profiler ----
// Records which functions were called and how many times during the current frame.
// On watchdog, the current profile is saved to a ring buffer of "latches."
// Queryable via TCP: 'profile' (current/latest latch), 'latches' (all saved).
#define PROFILE_MAX_FUNCS 256
#define PROFILE_TOP_N 10        // top callers saved per latch
#define LATCH_RING_SIZE 16      // remember last 16 watchdog profiles

typedef struct {
    const char *name;
    int call_count;
} ProfileEntry;

typedef struct {
    int frame_num;
    double frame_ms;
    int func_count;
    ProfileEntry top[PROFILE_TOP_N];
    int top_count;
} LatchedProfile;

// Current frame profiling state
static ProfileEntry s_profile[PROFILE_MAX_FUNCS];
static int s_profile_count = 0;
static volatile int s_profile_enabled = 0;
static volatile int s_profile_latched = 0;
static clock_t s_profile_frame_start;
static double s_profile_frame_ms;
static int s_profile_frame_num = -1;

// Latch ring buffer
static LatchedProfile s_latches[LATCH_RING_SIZE];
static int s_latch_write = 0;
static int s_latch_count = 0;

// ---- Global unique function tracker ----
// Records every unique function name ever called. Queryable via TCP 'get_functions'.
#define FUNC_TRACKER_MAX 2048
static const char *s_func_tracker[FUNC_TRACKER_MAX];
static int s_func_tracker_count = 0;

static void func_tracker_push(const char *name) {
    // Check if already tracked
    for (int i = 0; i < s_func_tracker_count; i++) {
        if (s_func_tracker[i] == name) return;  // pointer comparison (interned strings)
    }
    if (s_func_tracker_count < FUNC_TRACKER_MAX)
        s_func_tracker[s_func_tracker_count++] = name;
}

// Called from RecompStackPush when profiling is enabled
void debug_server_profile_push(const char *name) {
    func_tracker_push(name);  // always track, regardless of profiling state
#if SNESRECOMP_REVERSE_DEBUG
    debug_on_recomp_stack_push(name);
#endif
    if (!s_profile_enabled) return;
    for (int i = 0; i < s_profile_count; i++) {
        if (s_profile[i].name == name) {
            s_profile[i].call_count++;
            return;
        }
    }
    if (s_profile_count < PROFILE_MAX_FUNCS) {
        s_profile[s_profile_count].name = name;
        s_profile[s_profile_count].call_count = 1;
        s_profile_count++;
    }
}

// Called from watchdog handler — save profile snapshot to latch ring
void debug_server_profile_latch(int frame_num) {
    if (!s_profile_enabled) return;
    double ms = (double)(clock() - s_profile_frame_start) * 1000.0 / CLOCKS_PER_SEC;
    s_profile_frame_ms = ms;
    s_profile_frame_num = frame_num;
    s_profile_latched = 1;

    // Save to ring buffer with top N callers
    LatchedProfile *lp = &s_latches[s_latch_write % LATCH_RING_SIZE];
    lp->frame_num = frame_num;
    lp->frame_ms = ms;
    lp->func_count = s_profile_count;
    lp->top_count = 0;

    // Extract top N by call count
    int used[PROFILE_MAX_FUNCS] = {0};
    for (int t = 0; t < PROFILE_TOP_N && t < s_profile_count; t++) {
        int best = -1;
        for (int i = 0; i < s_profile_count; i++) {
            if (!used[i] && (best < 0 || s_profile[i].call_count > s_profile[best].call_count))
                best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        lp->top[lp->top_count].name = s_profile[best].name;
        lp->top[lp->top_count].call_count = s_profile[best].call_count;
        lp->top_count++;
    }
    s_latch_write++;
    if (s_latch_count < LATCH_RING_SIZE) s_latch_count++;
    fprintf(stderr, "  [profile] LATCH frame=%d %.0fms %d funcs (latch %d/%d)\n",
            frame_num, ms, s_profile_count, s_latch_count, LATCH_RING_SIZE);
}

// ---- Frame history ring buffer ----
// Stores per-frame data for retroactive queries (10 min @ 60fps = 36000 frames).
// Each frame records: pass/fail, ptr sync status, diff summary, last func,
// and a snapshot of key game state bytes for cross-server comparison.
// Ring buffer sizing tradeoff: capturing full WRAM (128KB) + full VRAM
// (64KB) per frame is ~196KB. At FRAME_HISTORY_SIZE=6000 that's ~1.2GB
// resident — enough for ~100 seconds of full-state history. A larger
// ring (e.g. the previous 36000-frame / 10-minute target) multiplied
// by 196KB becomes 7GB+ which exceeds reasonable dev-machine budgets
// and Windows MSVC static-array linker limits. If you need a longer
// window, either (a) bump this and accept the memory cost, or (b)
// split into separate rings (a 36000-frame small-state ring + a
// smaller big-state ring). The small-state ring still holds ~100s
// of every-frame CPU/PPU/DMA/CGRAM/OAM/zeropage/wram_1000 without
// the 196KB/frame adds.
#define FRAME_HISTORY_SIZE 6000

// Key RAM addresses snapshotted each frame (must match oracle debug_server.c)
#define SNAP_BYTES 64
static const uint16_t s_snap_addrs[SNAP_BYTES] = {
    // DP scratch / core state (0x00-0x0F)
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    // Map16 pointer bytes
    0x6B, 0x6C, 0x6E, 0x6F, 0x70,
    // Game mode and GFX file slots (0x100-0x10A)
    0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10A,
    // Misc game state
    0xD1, 0xD2,   // layer1 x/y scroll
    0xD3, 0xD4,   // layer1 x/y scroll high
    0xD9, 0xDA, 0xDB, 0xDC,  // BG scroll positions
    0x13BF,       // translevel number
    0x1426,       // overworld flag
    0x141A,       // bonus stars
    0x0D9B,       // current level number
    0x1F11, 0x1F12,  // sublevel number
    0x71, 0x72,   // player state
    0x7E, 0x7F,   // misc
    0x1BA1,       // blocks screen counter
    0x1928,       // blocks screen counter 2
    // GFX decompress targets
    0xAD00, 0xAD01,  // first two bytes of GFX buffer
    // Level loading diagnostics
    0x0E, 0x0F,       // scratch regs used for level pointer index
    0x0DB4,            // ow_players_map[0]
    0x5A,              // blocks_object_number
    0x65, 0x66, 0x67,  // ptr_layer1_data
    0, 0
};

// Per-frame CPU register snapshot (16 bytes)
typedef struct {
    uint16_t a, x, y, sp, pc, dp;
    uint8_t k, db;
    uint8_t flags;  // packed: bit0=c,1=z,2=v,3=n,4=i,5=d,6=xf,7=mf
    uint8_t e;      // emulation mode
} FrameCpuSnap;

// Per-frame PPU register snapshot (32 bytes)
typedef struct {
    uint8_t inidisp, bgmode, mosaic, obsel, setini;
    uint8_t screenEnabled[2], cgadsub, cgwsel, pad;
    uint16_t hScroll[4], vScroll[4];
    uint16_t fixedColor, vramPointer;
} FramePpuSnap;

// Per-frame interrupt/timing snapshot. Added 2026-04-23 after the tooling-
// audit found interrupt state completely unobservable post-interpreter-rip:
// the Cpu struct's nmiWanted/irqWanted were deleted as write-only, but the
// SNES struct's inNmi/inIrq/inVblank/timers are live and load-bearing.
typedef struct {
    uint8_t inNmi;          // currently servicing NMI
    uint8_t inIrq;          // currently servicing IRQ
    uint8_t inVblank;       // inside vblank interval
    uint8_t nmiEnabled;     // $4200 bit 7
    uint8_t hIrqEnabled;    // $4200 bit 4
    uint8_t vIrqEnabled;    // $4200 bit 5
    uint8_t autoJoyRead;    // $4200 bit 0
    uint8_t _pad;
    uint16_t hPos;          // current H dot
    uint16_t vPos;          // current V scanline
    uint16_t hTimer;        // $4207/$4208
    uint16_t vTimer;        // $4209/$420A
    uint16_t autoJoyTimer;
    uint16_t _pad2;
} FrameIrqSnap;

// Per-frame DMA channel snapshot (16 bytes per channel, incl. HDMA state).
// HDMA fields (tableAdr, repCount, indBank, offIndex, doTransfer, terminated)
// were added 2026-04-23 after the tooling-audit found HDMA was invisible in
// frame history — any bug involving per-scanline HDMA sequencing was previously
// undiagnosable from historical snapshots alone.
typedef struct {
    uint8_t bAdr, aBank, mode, flags;
    // flags: bit0=dmaActive, 1=hdmaActive, 2=fixed, 3=decrement,
    //        4=indirect, 5=fromB, 6=doTransfer, 7=terminated
    uint16_t aAdr, size;
    uint16_t tableAdr;   // HDMA table pointer
    uint8_t indBank;     // HDMA indirect bank
    uint8_t repCount;    // HDMA line-repeat counter
    uint8_t offIndex;    // HDMA offset index into table
    uint8_t _pad[3];     // keep struct size a multiple of 8 bytes
} FrameDmaChannelSnap;

typedef struct {
    int frame_number;
    char last_func[64];
    uint8_t snap[SNAP_BYTES]; // key game state snapshot for cross-server comparison
    // --- Extended state (added for exhaustive comparison) ---
    FrameCpuSnap cpu;
    FramePpuSnap ppu;
    FrameDmaChannelSnap dma[8];
    FrameIrqSnap irq;
    uint16_t cgram[0x100];    // 512 bytes (full palette)
    uint16_t oam[0x100];      // 512 bytes (main OAM table)
    uint8_t highOam[0x20];    // 32 bytes (high OAM table)
    uint8_t zeropage[256];    // 256 bytes (WRAM $00-$FF) — retained for backward-compat with tools that read it directly
    uint8_t wram_1000[4096];  // 4096 bytes (WRAM $1000-$1FFF) — retained for backward-compat
    // Full state captures (added 2026-04-18 per ring-buffer-is-principal-
    // observability principle). Any address range that was previously only
    // queryable on-demand (dump_ram, dump_vram) is now also in the ring
    // for historical queries. zeropage/wram_1000 are now subsets of wram.
    uint8_t wram[0x20000];    // 128 KB — full SNES WRAM ($7E0000-$7FFFFF)
    uint8_t vram[0x10000];    // 64 KB  — full SNES VRAM ($0000-$FFFF word-addressable × 2)
} FrameRecord;

static FrameRecord s_frame_history[FRAME_HISTORY_SIZE];
static int s_history_write_idx = 0;
static int s_history_count = 0;

// Called from the verify system after each frame comparison (main thread).
// Protected by mutex since the network thread reads frame history.
void debug_server_record_frame(int frame) {
    extern uint8_t g_ram[];

    // Step counter: auto-re-pause after N frames
    if (s_step_remaining > 0) {
        if (--s_step_remaining == 0) {
            s_paused = 1;
        }
    }

    lock_mutex();

    FrameRecord *r = &s_frame_history[s_history_write_idx];
    r->frame_number = frame;

    // Record last function
    if (g_last_recomp_func)
        strncpy(r->last_func, g_last_recomp_func, sizeof(r->last_func) - 1);
    else
        strcpy(r->last_func, "?");
    r->last_func[sizeof(r->last_func) - 1] = 0;

    // Snapshot key game state bytes for cross-server comparison
    for (int i = 0; i < SNAP_BYTES; i++) {
        uint16_t a = s_snap_addrs[i];
        r->snap[i] = (a < s_ram_size && s_ram) ? s_ram[a] : 0;
    }

    // --- Extended state snapshots ---

    // CPU registers
    if (g_snes_cpu) {
        r->cpu.a = g_snes_cpu->a;
        r->cpu.x = g_snes_cpu->x;
        r->cpu.y = g_snes_cpu->y;
        r->cpu.sp = g_snes_cpu->sp;
        r->cpu.pc = g_snes_cpu->pc;
        r->cpu.dp = g_snes_cpu->dp;
        r->cpu.k = g_snes_cpu->k;
        r->cpu.db = g_snes_cpu->db;
        r->cpu.flags = (g_snes_cpu->c ? 1 : 0) | (g_snes_cpu->z ? 2 : 0) | (g_snes_cpu->v ? 4 : 0) |
                        (g_snes_cpu->n ? 8 : 0) | (g_snes_cpu->i ? 16 : 0) | (g_snes_cpu->d ? 32 : 0) |
                        (g_snes_cpu->xf ? 64 : 0) | (g_snes_cpu->mf ? 128 : 0);
        r->cpu.e = g_snes_cpu->e ? 1 : 0;
    } else {
        memset(&r->cpu, 0, sizeof(r->cpu));
    }

    // PPU registers
    if (g_ppu) {
        r->ppu.inidisp = g_ppu->inidisp;
        r->ppu.bgmode = g_ppu->bgmode;
        r->ppu.mosaic = g_ppu->mosaic;
        r->ppu.obsel = g_ppu->obsel;
        r->ppu.setini = g_ppu->setini;
        r->ppu.screenEnabled[0] = g_ppu->screenEnabled[0];
        r->ppu.screenEnabled[1] = g_ppu->screenEnabled[1];
        r->ppu.cgadsub = g_ppu->cgadsub;
        r->ppu.cgwsel = g_ppu->cgwsel;
        r->ppu.pad = 0;
        memcpy(r->ppu.hScroll, g_ppu->hScroll, sizeof(r->ppu.hScroll));
        memcpy(r->ppu.vScroll, g_ppu->vScroll, sizeof(r->ppu.vScroll));
        r->ppu.fixedColor = g_ppu->fixedColor;
        r->ppu.vramPointer = g_ppu->vramPointer;
        // CGRAM + OAM snapshots
        memcpy(r->cgram, g_ppu->cgram, sizeof(r->cgram));
        memcpy(r->oam, g_ppu->oam, sizeof(r->oam));
        memcpy(r->highOam, g_ppu->highOam, sizeof(r->highOam));
    } else {
        memset(&r->ppu, 0, sizeof(r->ppu));
        memset(r->cgram, 0, sizeof(r->cgram));
        memset(r->oam, 0, sizeof(r->oam));
        memset(r->highOam, 0, sizeof(r->highOam));
    }

    // DMA channels (incl. HDMA state)
    if (g_dma) {
        for (int ch = 0; ch < 8; ch++) {
            DmaChannel *dc = &g_dma->channel[ch];
            r->dma[ch].bAdr = dc->bAdr;
            r->dma[ch].aBank = dc->aBank;
            r->dma[ch].mode = dc->mode;
            r->dma[ch].flags = (dc->dmaActive   ? 0x01 : 0)
                             | (dc->hdmaActive  ? 0x02 : 0)
                             | (dc->fixed       ? 0x04 : 0)
                             | (dc->decrement   ? 0x08 : 0)
                             | (dc->indirect    ? 0x10 : 0)
                             | (dc->fromB       ? 0x20 : 0)
                             | (dc->doTransfer  ? 0x40 : 0)
                             | (dc->terminated  ? 0x80 : 0);
            r->dma[ch].aAdr = dc->aAdr;
            r->dma[ch].size = dc->size;
            r->dma[ch].tableAdr = dc->tableAdr;
            r->dma[ch].indBank  = dc->indBank;
            r->dma[ch].repCount = dc->repCount;
            r->dma[ch].offIndex = dc->offIndex;
        }
    } else {
        memset(r->dma, 0, sizeof(r->dma));
    }

    // Interrupt / timing state
    if (g_snes) {
        r->irq.inNmi       = g_snes->inNmi       ? 1 : 0;
        r->irq.inIrq       = g_snes->inIrq       ? 1 : 0;
        r->irq.inVblank    = g_snes->inVblank    ? 1 : 0;
        r->irq.nmiEnabled  = g_snes->nmiEnabled  ? 1 : 0;
        r->irq.hIrqEnabled = g_snes->hIrqEnabled ? 1 : 0;
        r->irq.vIrqEnabled = g_snes->vIrqEnabled ? 1 : 0;
        r->irq.autoJoyRead = g_snes->autoJoyRead ? 1 : 0;
        r->irq.hPos         = g_snes->hPos;
        r->irq.vPos         = g_snes->vPos;
        r->irq.hTimer       = g_snes->hTimer;
        r->irq.vTimer       = g_snes->vTimer;
        r->irq.autoJoyTimer = g_snes->autoJoyTimer;
    } else {
        memset(&r->irq, 0, sizeof(r->irq));
    }

    // Zero page snapshot (WRAM $00-$FF) — backward-compat alias.
    if (s_ram && s_ram_size >= 256)
        memcpy(r->zeropage, s_ram, 256);
    else
        memset(r->zeropage, 0, 256);

    // Game state WRAM snapshot ($1000-$1FFF) — backward-compat alias.
    if (s_ram && s_ram_size >= 0x2000)
        memcpy(r->wram_1000, s_ram + 0x1000, 4096);
    else
        memset(r->wram_1000, 0, 4096);

    // Full WRAM snapshot ($0-$1FFFF, 128KB). Source of truth; the two
    // back-compat subsets above are redundant with this.
    if (s_ram && s_ram_size >= 0x20000)
        memcpy(r->wram, s_ram, 0x20000);
    else {
        memset(r->wram, 0, 0x20000);
        if (s_ram && s_ram_size > 0)
            memcpy(r->wram, s_ram, s_ram_size < 0x20000 ? s_ram_size : 0x20000);
    }

    // Full VRAM snapshot (64KB word-addressable → stored as raw bytes).
    if (g_ppu)
        memcpy(r->vram, g_ppu->vram, 0x10000);
    else
        memset(r->vram, 0, 0x10000);

    s_history_write_idx = (s_history_write_idx + 1) % FRAME_HISTORY_SIZE;
    if (s_history_count < FRAME_HISTORY_SIZE) s_history_count++;

    unlock_mutex();
}

// Find a frame record by frame number. Returns NULL if not in buffer.
static FrameRecord *find_frame(int frame_num) {
    // Search backward from most recent
    for (int i = 0; i < s_history_count; i++) {
        int idx = (s_history_write_idx - 1 - i + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE;
        if (s_frame_history[idx].frame_number == frame_num)
            return &s_frame_history[idx];
    }
    return NULL;
}

static char s_recv_buf[4096];
static int s_recv_len = 0;

static void set_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ===== Always-on OAM observability (widescreen sprite-margin diagnosis) =====
//
// Two always-on rings answer one question precisely: "when/where does
// ppu->oam receive each frame's real sprite tiles, vs when does ppu_runLine
// read them at render?" No arming — both rings record continuously from the
// moment debug_server_init() runs. A single monotonic sequence counter
// (s_oam_seq) stamps every event in BOTH rings, so OAM writes (DMA bursts and
// stray CPU pokes) and render-reads share one total order: a write with a
// LOWER seq than the render-read that consumes the same frame landed first.
//
//   - write ring : every word landed in ppu->oam (low table) or byte landed
//     in ppu->highOam (high table). OAM DMA routes $2104 through ppu_write,
//     so DMA fills are captured too; g_last_recomp_func distinguishes the
//     OAM-DMA burst from any direct CPU store.
//   - render ring: a compact 128-slot snapshot of ppu->oam taken at the top
//     of ppu_runLine(line 0) — exactly the bytes the scanline renderer is
//     about to consume. `active` counts slots whose Y is on/near screen
//     (Y < 0xE0); SMW parks unused slots at Y = 0xF0, so a near-zero active
//     count at render means ppu->oam holds the cleared/parked buffer.
//
// Both rings are tiny (write ~30 MB, render ~170 KB) and unconditional
// (allocated whenever the debug server is built), unlike the REVERSE_DEBUG-
// gated VRAM rings. Probes connect → query [recent window] → analyze; they
// never arm-then-capture.

#define OAM_WRITE_RING_ENTRIES (512u * 1024u)
#define OAM_RENDER_RING_ENTRIES 256u

typedef struct {
    uint64_t seq;
    int      frame;
    uint8_t  is_high;   /* 0 = low-table word, 1 = high-table byte           */
    uint16_t index;     /* low: word index 0..255; high: byte index 0..31    */
    uint16_t value;     /* low: 16-bit word (hi=Y or attr, lo=Xlow or tile); */
                        /* high: byte value                                  */
    char     func[40];  /* g_last_recomp_func at write time                  */
} OamWriteEntry;

static struct {
    uint64_t write_idx;
    uint64_t count;
    OamWriteEntry *log; /* calloc'd at init */
} s_oam_wr = {0};

typedef struct { uint8_t y, xlow, tile, attr, xhigh; } OamSlotSnap;

typedef struct {
    uint64_t    seq;
    int         frame;
    int         active;       /* # slots with Y < 0xE0 (not parked off-screen) */
    OamSlotSnap slot[128];
} OamRenderEntry;

static struct {
    uint64_t write_idx;
    uint64_t count;
    OamRenderEntry *log;      /* calloc'd at init */
} s_oam_rd = {0};

static uint64_t s_oam_seq = 0;

void debug_server_on_oam_write(int is_high, uint16_t index, uint16_t value) {
    if (!s_oam_wr.log) return;
    uint64_t i = s_oam_wr.write_idx % OAM_WRITE_RING_ENTRIES;
    OamWriteEntry *e = &s_oam_wr.log[i];
    e->seq = s_oam_seq++;
    e->frame = snes_frame_counter;
    e->is_high = (uint8_t)(is_high ? 1 : 0);
    e->index = index;
    e->value = value;
    if (g_last_recomp_func) { strncpy(e->func, g_last_recomp_func, 39); e->func[39] = 0; }
    else e->func[0] = 0;
    s_oam_wr.write_idx++;
    if (s_oam_wr.count < OAM_WRITE_RING_ENTRIES) s_oam_wr.count++;
}

void debug_server_on_oam_render(void) {
    if (!s_oam_rd.log || !g_ppu) return;
    uint64_t i = s_oam_rd.write_idx % OAM_RENDER_RING_ENTRIES;
    OamRenderEntry *e = &s_oam_rd.log[i];
    e->seq = s_oam_seq++;
    e->frame = snes_frame_counter;
    int active = 0;
    for (int s = 0; s < 128; s++) {
        uint16_t w0 = g_ppu->oam[s * 2];      /* (Y << 8) | Xlow      */
        uint16_t w1 = g_ppu->oam[s * 2 + 1];  /* (attr << 8) | tile   */
        uint8_t y = (uint8_t)(w0 >> 8);
        e->slot[s].y     = y;
        e->slot[s].xlow  = (uint8_t)(w0 & 0xff);
        e->slot[s].tile  = (uint8_t)(w1 & 0xff);
        e->slot[s].attr  = (uint8_t)(w1 >> 8);
        e->slot[s].xhigh = (uint8_t)((g_ppu->highOam[s >> 2] >> ((s & 3) * 2)) & 1);
        if (y < 0xE0) active++;
    }
    e->active = active;
    s_oam_rd.write_idx++;
    if (s_oam_rd.count < OAM_RENDER_RING_ENTRIES) s_oam_rd.count++;
}

static void send_line(const char *line) {
    if (s_client_sock == SOCKET_INVALID) return;
    send(s_client_sock, line, (int)strlen(line), 0);
    send(s_client_sock, "\n", 1, 0);
}

static void send_fmt(const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_line(buf);
}

// ---- Command handlers ----

static void cmd_ping(const char *args) {
    send_fmt("{\"ok\":true,\"frame\":%d}", snes_frame_counter);
}

static void cmd_frame(const char *args) {
    send_fmt("{\"frame\":%d,\"func\":\"%s\"}", snes_frame_counter,
             g_last_recomp_func ? g_last_recomp_func : "?");
}

// read_ram: space-separated hex, streamed to handle arbitrary lengths up to
// full WRAM (128 KB). Format kept for back-compat with existing probe scripts
// that parse r['hex'].split(). Prior implementation silently clamped to 1024
// bytes against a fixed 4 KB hex buffer, which masked divergences in any
// probe requesting a larger range (most notably _probe_bug8_full_wram_diff.py
// asking for 0x2000 bytes and only comparing the first 0x400).
static void cmd_read_ram(const char *args) {
    unsigned int addr = 0, len = 16;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 0x20000) len = 0x20000;  // 128 KB max — full WRAM
    if (!s_ram || addr + len > s_ram_size) {
        send_fmt("{\"error\":\"out of range\",\"addr\":\"0x%x\",\"max\":\"0x%x\"}", addr, s_ram_size);
        return;
    }
    if (s_client_sock == SOCKET_INVALID) return;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"", addr, len);
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%s%02x", (i == 0) ? "" : " ", s_ram[addr + i]);
        send(s_client_sock, chunk, pos, 0);
    }
    send(s_client_sock, "\"}\n", 3, 0);
}

// dump_ram: compact (no-space) hex dump for oracle comparison. Same streaming
// shape as read_ram; the two differ only in format (space-separated vs. tight).
// Usage: dump_ram <start_hex> <len_decimal>
static void cmd_dump_ram(const char *args) {
    unsigned int addr = 0, len = 256;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 0x20000) len = 0x20000;  // 128 KB max — full WRAM
    if (!s_ram || addr + len > s_ram_size) {
        send_fmt("{\"error\":\"out of range\",\"addr\":\"0x%x\",\"len\":%u}", addr, len);
        return;
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"", addr, len);
    if (s_client_sock == SOCKET_INVALID) return;
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x", s_ram[addr + i]);
        send(s_client_sock, chunk, pos, 0);
    }
    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_read_sram(const char *args) {
    unsigned int addr = 0, len = 16;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 0x20000) len = 0x20000;
    if (!g_sram || addr + len > (unsigned int)g_sram_size) {
        send_fmt("{\"error\":\"out of range\",\"addr\":\"0x%x\",\"max\":\"0x%x\"}",
                 addr, g_sram_size);
        return;
    }
    if (s_client_sock == SOCKET_INVALID) return;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"", addr, len);
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%s%02x",
                            (i == 0) ? "" : " ", g_sram[addr + i]);
        send(s_client_sock, chunk, pos, 0);
    }
    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_call_stack(const char *args) {
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "{\"depth\":%d,\"stack\":[", g_recomp_stack_top);
    for (int i = g_recomp_stack_top - 1; i >= 0 && pos < 2000; i--)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"%s\"", i < g_recomp_stack_top - 1 ? "," : "",
                        g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_watch(const char *args) {
    unsigned int addr = 0;
    sscanf(args, "%x", &addr);
    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) {
            s_watchpoints[i].addr = addr;
            s_watchpoints[i].prev_val = s_ram ? s_ram[addr] : 0;
            s_watchpoints[i].active = 1;
            send_fmt("{\"ok\":true,\"slot\":%d,\"addr\":\"0x%x\"}", i, addr);
            return;
        }
    }
    send_fmt("{\"error\":\"no free watchpoint slots\"}");
}

static void cmd_unwatch(const char *args) {
    unsigned int addr = 0;
    sscanf(args, "%x", &addr);
    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (s_watchpoints[i].active && s_watchpoints[i].addr == addr) {
            s_watchpoints[i].active = 0;
            send_fmt("{\"ok\":true,\"cleared\":\"0x%x\"}", addr);
            return;
        }
    }
    send_fmt("{\"error\":\"watchpoint not found\"}");
}

static void cmd_pause(const char *args) {
    // No-op by design. Pausing the simulation to "freeze the ring for query"
    // is the same anti-pattern as arm-then-capture: it synchronizes the
    // observer with the system instead of querying the always-on ring for
    // the window of interest. Rings are sized to cover realistic query
    // latency; if they aren't large enough, enlarge the ring, do not pause.
    (void)args;
    send_fmt("{\"ok\":false,\"error\":\"pause is disabled by policy; query the always-on rings for the window of interest. If the ring is too small, enlarge it.\",\"frame\":%d}", snes_frame_counter);
}

static void cmd_continue(const char *args) {
    s_paused = 0;
    send_fmt("{\"ok\":true,\"paused\":false}");
}

/* func_snap_set <name>     — register a function name to snapshot.
 *                             Subsequent calls populate a 256-deep
 *                             ring buffer with 8KB WRAM slices.
 * func_snap_count           — { count, frame } of latest entry.
 * func_snap_get_n <call_idx> <hex_addr> [len]
 *                          — fetch slice from the snapshot of the
 *                             given absolute call index. Errors if
 *                             call_idx is no longer in the ring or
 *                             addr+len exceeds 8KB.
 * Empty name disables the snapshot. */
typedef struct { int call_idx; int frame; uint8_t wram_slice[0x2000]; } recomp_snap_entry;
extern const char *g_recomp_snap_on_func;
extern int        g_recomp_snap_count;
extern int        g_recomp_snap_frame;
extern recomp_snap_entry g_recomp_snap_ring[256];
extern const recomp_snap_entry* recomp_snap_lookup(int call_idx);

static char s_snap_name_buf[128];

static void cmd_func_snap_set(const char *args) {
    if (!args || !args[0]) {
        g_recomp_snap_on_func = NULL;
        g_recomp_snap_count = 0;
        g_recomp_snap_frame = -1;
        send_fmt("{\"ok\":true,\"cleared\":true}");
        return;
    }
    int n = 0;
    while (args[n] && args[n] != '\n' && args[n] != '\r' && n < 127) {
        s_snap_name_buf[n] = args[n];
        n++;
    }
    s_snap_name_buf[n] = 0;
    g_recomp_snap_on_func = s_snap_name_buf;
    g_recomp_snap_count = 0;
    g_recomp_snap_frame = -1;
    send_fmt("{\"ok\":true,\"watching\":\"%s\"}", s_snap_name_buf);
}

static void cmd_func_snap_count(const char *args) {
    (void)args;
    send_fmt("{\"ok\":true,\"count\":%d,\"frame\":%d,\"ring_len\":256}",
             g_recomp_snap_count, g_recomp_snap_frame);
}

static void cmd_func_snap_get_n(const char *args) {
    int call_idx = -1;
    unsigned int addr = 0, len = 1;
    if (!args || sscanf(args, "%d %x %u", &call_idx, &addr, &len) < 2) {
        send_fmt("{\"ok\":false,\"error\":\"usage: func_snap_get_n <call_idx> <hex_addr> [len]\"}");
        return;
    }
    const recomp_snap_entry *e = recomp_snap_lookup(call_idx);
    if (!e) {
        send_fmt("{\"ok\":false,\"error\":\"call_idx %d not in ring\","
                 "\"current_count\":%d}",
                 call_idx, g_recomp_snap_count);
        return;
    }
    if (len < 1) len = 1;
    if (len > 0x2000) len = 0x2000;
    if (addr >= 0x2000u || addr + len > 0x2000u) {
        send_fmt("{\"ok\":false,\"error\":\"addr+len exceeds 8KB slice\"}");
        return;
    }
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "{\"ok\":true,\"call_idx\":%d,\"frame\":%d,"
        "\"addr\":\"0x%04x\",\"len\":%u,\"hex\":\"",
        e->call_idx, e->frame, addr, len);
    if (s_client_sock == SOCKET_INVALID) return;
    send(s_client_sock, hdr, hlen, 0);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x",
                            e->wram_slice[addr + i]);
        send(s_client_sock, chunk, pos, 0);
    }
    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_step(const char *args) {
    int n = 1;
    if (args[0]) sscanf(args, "%d", &n);
    int start_frame = snes_frame_counter;
    s_step_remaining = n;
    s_paused = 0;
    /* Release the mutex so the main thread can run frames and call
     * debug_server_record_frame (which also needs the mutex for snapshotting).
     * Without releasing here, the main thread deadlocks: it decrements
     * s_step_remaining before the mutex lock, then blocks waiting for the
     * mutex the TCP thread still holds, so s_step_remaining never reaches 0.
     * Re-acquire before returning; caller (try_recv_and_process) expects it. */
    unlock_mutex();
    int waited = 0;
    while (s_step_remaining > 0) {
#ifdef _WIN32
        Sleep(1);                          /* 1 ms per iter; cap at 5 000 ms */
        if (++waited >= 5000) break;
#else
        struct timespec ts = {0, 30000};  /* 30 µs */
        nanosleep(&ts, NULL);
        if (++waited >= 150000) break;    /* cap at 4.5 s */
#endif
    }
    lock_mutex();
    send_fmt("{\"ok\":true,\"stepped\":%d,\"frame_before\":%d,\"frame_after\":%d%s}",
             n, start_frame, snes_frame_counter,
             (s_step_remaining > 0) ? ",\"timeout\":true" : "");
}

static void cmd_run_to_frame(const char *args) {
    int target = 0;
    sscanf(args, "%d", &target);
    if (target <= snes_frame_counter) {
        send_fmt("{\"error\":\"target frame %d <= current %d\"}", target, snes_frame_counter);
        return;
    }
    s_paused = 0;
    send_fmt("{\"ok\":true,\"running_to\":%d,\"current\":%d}", target, snes_frame_counter);
    // The poll function will re-pause when we reach the target
}

typedef struct ControllerName {
    const char *name;
    uint32_t mask;
} ControllerName;

static const ControllerName k_controller_names[] = {
    {"b",      0x0001u},
    {"y",      0x0002u},
    {"select", 0x0004u},
    {"start",  0x0008u},
    {"up",     0x0010u},
    {"down",   0x0020u},
    {"left",   0x0040u},
    {"right",  0x0080u},
    {"a",      0x0100u},
    {"x",      0x0200u},
    {"l",      0x0400u},
    {"r",      0x0800u},
    {NULL, 0}
};

static void lower_ascii_in_place(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
    }
}

static int parse_controller_mask(const char *text, uint32_t *out_mask,
                                 char *err, size_t err_size) {
    char buf[160];
    uint32_t mask = 0;
    if (!text || !*text) {
        snprintf(err, err_size, "empty controller mask");
        return 0;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    lower_ascii_in_place(buf);

    if (strcmp(buf, "none") == 0 || strcmp(buf, "release") == 0 ||
        strcmp(buf, "released") == 0 || strcmp(buf, "off") == 0 ||
        strcmp(buf, "0") == 0) {
        *out_mask = 0;
        return 1;
    }

    if (buf[0] == '0' && buf[1] == 'x') {
        char *end = NULL;
        unsigned long v = strtoul(buf, &end, 16);
        if (!end || *end) {
            snprintf(err, err_size, "bad hex controller mask '%s'", text);
            return 0;
        }
        *out_mask = (uint32_t)(v & 0x0FFFu);
        return 1;
    }

    char *p = buf;
    while (*p) {
        char *tok = p;
        while (*p && *p != '+' && *p != ',' && *p != '|') p++;
        if (*p) *p++ = 0;

        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (*tok) {
            int found = 0;
            for (const ControllerName *n = k_controller_names; n->name; n++) {
                if (strcmp(tok, n->name) == 0) {
                    mask |= n->mask;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                snprintf(err, err_size, "unknown controller button '%s'", tok);
                return 0;
            }
        }
    }

    *out_mask = mask;
    return 1;
}

static void emit_controller_state(void) {
    uint32_t p1 = s_controller_inputs & 0x0FFFu;
    uint32_t p2 = (s_controller_inputs >> 12) & 0x0FFFu;
    uint32_t active = s_controller_active & 0x3u;
    send_fmt("{\"ok\":true,\"p1\":\"0x%03x\",\"p2\":\"0x%03x\","
             "\"p1_active\":%s,\"p2_active\":%s}",
             p1, p2,
             (active & 1u) ? "true" : "false",
             (active & 2u) ? "true" : "false");
}

static void cmd_set_controller(const char *args) {
    if (!args || !*args) {
        send_fmt("{\"ok\":false,\"error\":\"usage: set_controller "
                 "<p1_buttons|0xmask> [p2_buttons|0xmask]\"}");
        return;
    }

    char buf[256];
    char err[128] = {0};
    uint32_t p1 = 0, p2 = 0;
    int p1_set = 0, p2_set = 0;
    int positional = 0;
    snprintf(buf, sizeof(buf), "%s", args);

    char *tok = strtok(buf, " \t\r\n");
    while (tok) {
        const char *value = tok;
        if (strncmp(tok, "p1=", 3) == 0 || strncmp(tok, "P1=", 3) == 0) {
            value = tok + 3;
            if (!parse_controller_mask(value, &p1, err, sizeof(err))) {
                send_fmt("{\"ok\":false,\"error\":\"%s\"}", err);
                return;
            }
            p1_set = 1;
        } else if (strncmp(tok, "p2=", 3) == 0 || strncmp(tok, "P2=", 3) == 0) {
            value = tok + 3;
            if (!parse_controller_mask(value, &p2, err, sizeof(err))) {
                send_fmt("{\"ok\":false,\"error\":\"%s\"}", err);
                return;
            }
            p2_set = 1;
        } else if (positional == 0) {
            if (!parse_controller_mask(value, &p1, err, sizeof(err))) {
                send_fmt("{\"ok\":false,\"error\":\"%s\"}", err);
                return;
            }
            p1_set = 1;
            positional++;
        } else if (positional == 1) {
            if (!parse_controller_mask(value, &p2, err, sizeof(err))) {
                send_fmt("{\"ok\":false,\"error\":\"%s\"}", err);
                return;
            }
            p2_set = 1;
            positional++;
        } else {
            send_fmt("{\"ok\":false,\"error\":\"too many controller args\"}");
            return;
        }
        tok = strtok(NULL, " \t\r\n");
    }

    if (!p1_set && !p2_set) {
        send_fmt("{\"ok\":false,\"error\":\"no controller mask supplied\"}");
        return;
    }

    s_controller_inputs = (p1 & 0x0FFFu) | ((p2 & 0x0FFFu) << 12);
    s_controller_active = (p1_set ? 1u : 0u) | (p2_set ? 2u : 0u);
    emit_controller_state();
}

static void cmd_get_controller(const char *args) {
    (void)args;
    emit_controller_state();
}

static void cmd_clear_controller(const char *args) {
    (void)args;
    s_controller_inputs = 0;
    s_controller_active = 0;
    emit_controller_state();
}

static void cmd_trace_addr(const char *args) {
    unsigned int addr = 0;
    sscanf(args, "%x", &addr);
    s_addr_trace.addr = addr;
    s_addr_trace.prev_val = s_ram ? s_ram[addr] : 0;
    s_addr_trace.write_idx = 0;
    s_addr_trace.count = 0;
    s_addr_trace.active = 1;
    send_fmt("{\"ok\":true,\"tracing\":\"0x%x\",\"initial\":\"0x%02x\"}", addr, s_addr_trace.prev_val);
}

static void cmd_get_trace(const char *args) {
    if (!s_addr_trace.active) {
        send_fmt("{\"error\":\"no trace active\"}");
        return;
    }
    char buf[65536];
    int pos = snprintf(buf, sizeof(buf),
        "{\"addr\":\"0x%x\",\"entries\":%d,\"log\":[",
        s_addr_trace.addr, s_addr_trace.count);
    int start = s_addr_trace.count < TRACE_LOG_SIZE ? 0 :
                s_addr_trace.write_idx - TRACE_LOG_SIZE;
    for (int i = 0; i < s_addr_trace.count && pos < 60000; i++) {
        int idx = (start + i) % TRACE_LOG_SIZE;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"old\":\"0x%02x\",\"new\":\"0x%02x\",\"func\":\"%s\",\"stack\":[",
            i ? "," : "",
            s_addr_trace.log[idx].frame,
            s_addr_trace.log[idx].old_val,
            s_addr_trace.log[idx].new_val,
            s_addr_trace.log[idx].func);
        for (int s = 0; s < s_addr_trace.log[idx].stack_depth; s++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", s ? "," : "",
                s_addr_trace.log[idx].stack[s] ? s_addr_trace.log[idx].stack[s] : "?");
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_trace_reg(const char *args) {
    unsigned int lo = 0, hi = 0;
    sscanf(args, "%x %x", &lo, &hi);
    if (hi < lo || hi > 0xffff) {
        send_fmt("{\"error\":\"bad range\"}"); return;
    }
    if (s_reg_trace.nranges >= MAX_TRACE_RANGES) {
        send_fmt("{\"error\":\"too many ranges (max %d) — call trace_reg_reset first\"}",
                 MAX_TRACE_RANGES); return;
    }
    s_reg_trace.ranges[s_reg_trace.nranges].lo = (uint16_t)lo;
    s_reg_trace.ranges[s_reg_trace.nranges].hi = (uint16_t)hi;
    s_reg_trace.nranges++;
    s_reg_trace.active = 1;
    send_fmt("{\"ok\":true,\"lo\":\"0x%04x\",\"hi\":\"0x%04x\",\"nranges\":%d}",
             lo, hi, s_reg_trace.nranges);
}

static void cmd_trace_reg_reset(const char *args) {
    (void)args;
    s_reg_trace.nranges = 0;
    s_reg_trace.write_idx = 0;
    s_reg_trace.count = 0;
    s_reg_trace.active = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_trace_vram(const char *args) {
    unsigned int lo = 0, hi = 0;
    sscanf(args, "%x %x", &lo, &hi);
    if (hi < lo || hi > 0xffff) {
        send_fmt("{\"error\":\"bad range\"}"); return;
    }
    if (s_vram_trace.nranges >= MAX_VRAM_TRACE_RANGES) {
        send_fmt("{\"error\":\"too many ranges (max %d) — call trace_vram_reset first\"}",
                 MAX_VRAM_TRACE_RANGES); return;
    }
    s_vram_trace.ranges[s_vram_trace.nranges].lo = (uint16_t)lo;
    s_vram_trace.ranges[s_vram_trace.nranges].hi = (uint16_t)hi;
    s_vram_trace.nranges++;
    s_vram_trace.active = 1;
    send_fmt("{\"ok\":true,\"lo\":\"0x%04x\",\"hi\":\"0x%04x\",\"nranges\":%d}",
             lo, hi, s_vram_trace.nranges);
}

static void cmd_trace_vram_reset(const char *args) {
    (void)args;
    s_vram_trace.nranges = 0;
    s_vram_trace.write_idx = 0;
    s_vram_trace.count = 0;
    s_vram_trace.active = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_get_vram_trace(const char *args) {
    if (!s_vram_trace.active) {
        send_fmt("{\"error\":\"no vram trace active\"}"); return;
    }
    int nostack = args && strstr(args, "nostack") != NULL;
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf), "{\"ranges\":[");
    for (int i = 0; i < s_vram_trace.nranges; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s[\"0x%04x\",\"0x%04x\"]", i ? "," : "",
            s_vram_trace.ranges[i].lo, s_vram_trace.ranges[i].hi);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"entries\":%llu,\"log\":[",
        (unsigned long long)s_vram_trace.count);
    uint64_t cap = s_vram_trace.capacity ? s_vram_trace.capacity : 1;
    uint64_t start = s_vram_trace.count < cap ?
                     0 : s_vram_trace.write_idx - cap;
    int budget = (int)sizeof(buf) - 4096;
    for (uint64_t i = 0; i < s_vram_trace.count && pos < budget; i++) {
        uint64_t idx = (start + i) % cap;
        if (nostack) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"f\":%d,\"adr_byte\":\"0x%04x\",\"val\":\"0x%02x\","
                "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
                "\"D\":\"0x%04x\",\"DB\":\"0x%02x\",\"P\":\"0x%02x\","
                "\"m\":%u,\"x\":%u,\"func\":\"%s\"}",
                i ? "," : "",
                s_vram_trace.log[idx].frame,
                s_vram_trace.log[idx].adr_byte,
                s_vram_trace.log[idx].val,
                s_vram_trace.log[idx].A,
                s_vram_trace.log[idx].X,
                s_vram_trace.log[idx].Y,
                s_vram_trace.log[idx].D,
                s_vram_trace.log[idx].DB,
                s_vram_trace.log[idx].P,
                (unsigned)s_vram_trace.log[idx].m_flag,
                (unsigned)s_vram_trace.log[idx].x_flag,
                s_vram_trace.log[idx].func);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"f\":%d,\"adr_byte\":\"0x%04x\",\"val\":\"0x%02x\","
                "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
                "\"D\":\"0x%04x\",\"DB\":\"0x%02x\",\"P\":\"0x%02x\","
                "\"m\":%u,\"x\":%u,\"func\":\"%s\",\"stack\":[",
                i ? "," : "",
                s_vram_trace.log[idx].frame,
                s_vram_trace.log[idx].adr_byte,
                s_vram_trace.log[idx].val,
                s_vram_trace.log[idx].A,
                s_vram_trace.log[idx].X,
                s_vram_trace.log[idx].Y,
                s_vram_trace.log[idx].D,
                s_vram_trace.log[idx].DB,
                s_vram_trace.log[idx].P,
                (unsigned)s_vram_trace.log[idx].m_flag,
                (unsigned)s_vram_trace.log[idx].x_flag,
                s_vram_trace.log[idx].func);
            for (int s = 0; s < s_vram_trace.log[idx].stack_depth; s++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s\"%s\"", s ? "," : "",
                    s_vram_trace.log[idx].stack[s] ? s_vram_trace.log[idx].stack[s] : "?");
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
        }
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* oam_state — ring depths + the current monotonic seq. Cheap heartbeat to
 * confirm the always-on OAM rings are recording. */
static void cmd_oam_state(const char *args) {
    (void)args;
    send_fmt("{\"ok\":true,\"frame\":%d,\"seq\":%llu,"
             "\"write_count\":%llu,\"write_cap\":%u,"
             "\"render_count\":%llu,\"render_cap\":%u}",
             snes_frame_counter, (unsigned long long)s_oam_seq,
             (unsigned long long)s_oam_wr.count, OAM_WRITE_RING_ENTRIES,
             (unsigned long long)s_oam_rd.count, OAM_RENDER_RING_ENTRIES);
}

/* oam_write_get [count=64] — most recent N OAM write events, oldest-first.
 * Each: seq, frame f, h=is_high, i=index, v=value (hex), func. The seq lets
 * you interleave these against oam_render_get to see whether the DMA burst
 * for a frame landed BEFORE the render-read that consumed it, and whether the
 * written Y bytes were real sprites or the parked 0xF0 clear buffer. */
static void cmd_oam_write_get(const char *args) {
    unsigned n = 64;
    if (args && *args) { unsigned v; if (sscanf(args, "%u", &v) == 1 && v) n = v; }
    if (!s_oam_wr.log) { send_fmt("{\"error\":\"oam write ring not allocated\"}"); return; }
    uint64_t have = s_oam_wr.count;
    if ((uint64_t)n > have) n = (unsigned)have;
    if (n > 4000) n = 4000;  /* wire/buffer cap */
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"frame\":%d,\"seq\":%llu,\"count\":%u,\"events\":[",
        snes_frame_counter, (unsigned long long)s_oam_seq, n);
    uint64_t start = s_oam_wr.write_idx - n;
    int budget = (int)sizeof(buf) - 256;
    for (unsigned k = 0; k < n && pos < budget; k++) {
        uint64_t idx = (start + k) % OAM_WRITE_RING_ENTRIES;
        OamWriteEntry *e = &s_oam_wr.log[idx];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"seq\":%llu,\"f\":%d,\"h\":%u,\"i\":%u,\"v\":\"0x%04x\",\"func\":\"%s\"}",
            k ? "," : "", (unsigned long long)e->seq, e->frame,
            (unsigned)e->is_high, (unsigned)e->index, (unsigned)e->value, e->func);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* oam_render_get [count=4] [slots=16] — most recent N render-time snapshots
 * (oldest-first). Each: seq, frame f, active count, and the first `slots`
 * OAM slots as [y,xlow,xhigh,tile,attr] (all decimal). This is the state the
 * scanline renderer actually consumed. active≈0 => ppu->oam was the parked
 * clear buffer at render. Default 16 slots keeps the payload small; pass a
 * larger slots= to inspect more. */
static void cmd_oam_render_get(const char *args) {
    unsigned n = 4, slots = 16;
    if (args && *args) { unsigned a, b; int got = sscanf(args, "%u %u", &a, &b);
        if (got >= 1 && a) n = a; if (got >= 2 && b) slots = b; }
    if (slots > 128) slots = 128;
    if (!s_oam_rd.log) { send_fmt("{\"error\":\"oam render ring not allocated\"}"); return; }
    uint64_t have = s_oam_rd.count;
    if ((uint64_t)n > have) n = (unsigned)have;
    if (n > OAM_RENDER_RING_ENTRIES) n = OAM_RENDER_RING_ENTRIES;
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"frame\":%d,\"seq\":%llu,\"count\":%u,\"slots\":%u,\"snaps\":[",
        snes_frame_counter, (unsigned long long)s_oam_seq, n, slots);
    uint64_t start = s_oam_rd.write_idx - n;
    int budget = (int)sizeof(buf) - 256;
    for (unsigned k = 0; k < n && pos < budget; k++) {
        uint64_t idx = (start + k) % OAM_RENDER_RING_ENTRIES;
        OamRenderEntry *e = &s_oam_rd.log[idx];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"seq\":%llu,\"f\":%d,\"active\":%d,\"slot\":[",
            k ? "," : "", (unsigned long long)e->seq, e->frame, e->active);
        for (unsigned s = 0; s < slots && pos < budget; s++)
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s[%u,%u,%u,%u,%u]", s ? "," : "",
                e->slot[s].y, e->slot[s].xlow, e->slot[s].xhigh,
                e->slot[s].tile, e->slot[s].attr);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* Walk the recomp VRAM byte-write ring BACKWARD from write_idx and
 * return the most recent entry whose byte_addr == requested address.
 * Avoids the "fetch entire ring; filter in Python" pattern that hits
 * the JSON-buffer cap of ~7400 entries.
 *
 * Usage: last_vram_write_to <byte_addr_hex>
 *
 * Reply (found):
 *   {"ok":true, "found":true, "f":F, "adr_byte":"0x..", "val":"0x..",
 *    "A":"0x..", "X":"0x..", "Y":"0x..", "D":"0x..", "DB":"0x..",
 *    "P":"0x..", "m":N, "x":N,
 *    "func":"...", "stack":[...]}
 *
 * Reply (none in ring):
 *   {"ok":true, "found":false, "ring_depth":N}
 */
static void cmd_last_vram_write_to(const char *args) {
    unsigned int target = 0;
    if (!args || sscanf(args, "%x", &target) != 1 || target > 0xFFFF) {
        send_fmt("{\"error\":\"usage: last_vram_write_to <byte_addr_hex> "
                 "(0..0xFFFF)\"}"); return;
    }
    if (!s_vram_trace.active) {
        send_fmt("{\"error\":\"recomp vram trace inactive\"}"); return;
    }
    uint16_t want = (uint16_t)target;
    uint64_t write_idx = s_vram_trace.write_idx;
    uint64_t depth = s_vram_trace.count;
    uint64_t cap = s_vram_trace.capacity ? s_vram_trace.capacity : 1;
    /* Walk backward from the most-recent entry. */
    for (uint64_t step = 1; step <= depth; step++) {
        uint64_t abs = write_idx - step;
        uint64_t idx = abs % cap;
        if (s_vram_trace.log[idx].adr_byte != want) continue;
        static char buf[8192];
        int pos = snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"found\":true,\"f\":%d,"
            "\"adr_byte\":\"0x%04x\",\"val\":\"0x%02x\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"D\":\"0x%04x\",\"DB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,"
            "\"func\":\"%s\",\"stack\":[",
            s_vram_trace.log[idx].frame,
            s_vram_trace.log[idx].adr_byte,
            s_vram_trace.log[idx].val,
            s_vram_trace.log[idx].A,
            s_vram_trace.log[idx].X,
            s_vram_trace.log[idx].Y,
            s_vram_trace.log[idx].D,
            s_vram_trace.log[idx].DB,
            s_vram_trace.log[idx].P,
            (unsigned)s_vram_trace.log[idx].m_flag,
            (unsigned)s_vram_trace.log[idx].x_flag,
            s_vram_trace.log[idx].func);
        for (int s = 0; s < s_vram_trace.log[idx].stack_depth; s++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", s ? "," : "",
                s_vram_trace.log[idx].stack[s] ?
                    s_vram_trace.log[idx].stack[s] : "?");
        }
        snprintf(buf + pos, sizeof(buf) - pos, "]}");
        send_line(buf);
        return;
    }
    send_fmt("{\"ok\":true,\"found\":false,\"ring_depth\":%llu}",
             (unsigned long long)depth);
}

/* Walk recomp + oracle VRAM byte-write rings forward in lockstep and
 * report the first divergent (byte_addr, value) pair within the
 * caller-supplied byte-address range. Entries outside the range are
 * skipped on each ring independently before the pair-up.
 *
 * Usage: vram_write_diff <lo_hex> <hi_hex>   (byte addresses)
 *
 * Reply (divergence):
 *   {"ok":true, "diverged":true, "first_diff_idx":N,
 *    "matched_pairs_before":N,
 *    "recomp":{"f":F,"adr_byte":"0x..","val":"0x..",
 *              "func":"...","stack":[...]},
 *    "oracle":{"f":F,"adr_byte":"0x..","val":"0x.."}}
 *
 * Reply (clean):
 *   {"ok":true, "diverged":false, "matched_pairs":N}
 *
 * Reply (one ring exhausted before any divergence inside range):
 *   {"ok":true, "diverged":false, "matched_pairs":N,
 *    "recomp_exhausted":true|false, "oracle_exhausted":true|false}
 */
static void cmd_vram_write_diff(const char *args) {
    unsigned int lo_u = 0, hi_u = 0;
    if (!args || sscanf(args, "%x %x", &lo_u, &hi_u) != 2 ||
        hi_u > 0xFFFF || hi_u < lo_u) {
        send_fmt("{\"error\":\"usage: vram_write_diff <lo_hex> <hi_hex> "
                 "(byte addresses, lo<=hi<=0xFFFF)\"}");
        return;
    }
    if (!s_vram_trace.active) {
        send_fmt("{\"error\":\"recomp vram trace inactive\"}"); return;
    }
    if (!s_oracle_vram_trace.active) {
        send_fmt("{\"error\":\"oracle vram trace inactive (Oracle build only)\"}");
        return;
    }
    uint16_t lo = (uint16_t)lo_u, hi = (uint16_t)hi_u;

    /* Ring base + bound for each side. The ring is a circular buffer:
     * entries [start, write_idx) are valid; older entries have been
     * evicted. We iterate by absolute index and modulo into the buffer
     * on each access. */
    uint64_t rcap = s_vram_trace.capacity ? s_vram_trace.capacity : 1;
    uint64_t ocap = s_oracle_vram_trace.capacity ? s_oracle_vram_trace.capacity : 1;
    uint64_t rec_start = s_vram_trace.count < rcap ?
                         0 : s_vram_trace.write_idx - rcap;
    uint64_t rec_end   = s_vram_trace.write_idx;
    uint64_t ora_start = s_oracle_vram_trace.count < ocap ?
                         0 : s_oracle_vram_trace.write_idx - ocap;
    uint64_t ora_end   = s_oracle_vram_trace.write_idx;

    uint64_t i_rec = rec_start;
    uint64_t i_ora = ora_start;
    uint64_t matched = 0;

    for (;;) {
        /* Advance recomp side to next in-range entry. */
        while (i_rec < rec_end) {
            uint64_t idx = i_rec % rcap;
            uint16_t a = s_vram_trace.log[idx].adr_byte;
            if (a >= lo && a <= hi) break;
            i_rec++;
        }
        /* Advance oracle side to next in-range entry. */
        while (i_ora < ora_end) {
            uint64_t idx = i_ora % ocap;
            uint16_t a = s_oracle_vram_trace.log[idx].adr_byte;
            if (a >= lo && a <= hi) break;
            i_ora++;
        }
        if (i_rec >= rec_end || i_ora >= ora_end) {
            /* One side exhausted with no divergence. */
            send_fmt("{\"ok\":true,\"diverged\":false,\"matched_pairs\":%llu,"
                     "\"recomp_exhausted\":%s,\"oracle_exhausted\":%s}",
                     (unsigned long long)matched,
                     i_rec >= rec_end ? "true" : "false",
                     i_ora >= ora_end ? "true" : "false");
            return;
        }
        uint64_t r_idx = i_rec % rcap;
        uint64_t o_idx = i_ora % ocap;
        uint16_t r_a = s_vram_trace.log[r_idx].adr_byte;
        uint8_t  r_v = s_vram_trace.log[r_idx].val;
        uint16_t o_a = s_oracle_vram_trace.log[o_idx].adr_byte;
        uint8_t  o_v = s_oracle_vram_trace.log[o_idx].val;
        if (r_a != o_a || r_v != o_v) {
            /* Mismatch — emit attribution-rich record. */
            static char buf[8192];
            int pos = snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"diverged\":true,\"first_diff_idx\":%llu,"
                "\"matched_pairs_before\":%llu,"
                "\"recomp\":{\"f\":%d,\"adr_byte\":\"0x%04x\","
                "\"val\":\"0x%02x\","
                "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
                "\"D\":\"0x%04x\",\"DB\":\"0x%02x\",\"P\":\"0x%02x\","
                "\"m\":%u,\"x\":%u,"
                "\"func\":\"%s\",\"stack\":[",
                (unsigned long long)matched,
                (unsigned long long)matched,
                s_vram_trace.log[r_idx].frame,
                r_a, r_v,
                s_vram_trace.log[r_idx].A,
                s_vram_trace.log[r_idx].X,
                s_vram_trace.log[r_idx].Y,
                s_vram_trace.log[r_idx].D,
                s_vram_trace.log[r_idx].DB,
                s_vram_trace.log[r_idx].P,
                (unsigned)s_vram_trace.log[r_idx].m_flag,
                (unsigned)s_vram_trace.log[r_idx].x_flag,
                s_vram_trace.log[r_idx].func);
            for (int s = 0; s < s_vram_trace.log[r_idx].stack_depth; s++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s\"%s\"", s ? "," : "",
                    s_vram_trace.log[r_idx].stack[s] ?
                        s_vram_trace.log[r_idx].stack[s] : "?");
            }
            snprintf(buf + pos, sizeof(buf) - pos,
                "]},\"oracle\":{\"f\":%d,\"adr_byte\":\"0x%04x\","
                "\"val\":\"0x%02x\"}}",
                s_oracle_vram_trace.log[o_idx].frame,
                o_a, o_v);
            send_line(buf);
            return;
        }
        matched++;
        i_rec++;
        i_ora++;
    }
}

/* Oracle-side VRAM byte-write ring reader. Returns ALL entries within
 * the JSON budget; entries are byte-addressed and lack func/stack
 * (snes9x is the reference, no attribution needed). Use the index
 * field to correlate with cmd_get_vram_trace's recomp-side log. */
static void cmd_get_oracle_vram_trace(const char *args) {
    (void)args;
    if (!s_oracle_vram_trace.active) {
        send_fmt("{\"error\":\"oracle vram trace inactive (Oracle build only)\"}");
        return;
    }
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf),
        "{\"entries\":%llu,\"write_idx\":%llu,\"log\":[",
        (unsigned long long)s_oracle_vram_trace.count,
        (unsigned long long)s_oracle_vram_trace.write_idx);
    uint64_t cap = s_oracle_vram_trace.capacity ? s_oracle_vram_trace.capacity : 1;
    uint64_t start = s_oracle_vram_trace.count < cap ?
                     0 :
                     s_oracle_vram_trace.write_idx - cap;
    int budget = (int)sizeof(buf) - 4096;
    for (uint64_t i = 0; i < s_oracle_vram_trace.count && pos < budget; i++) {
        uint64_t idx = (start + i) % cap;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"adr_byte\":\"0x%04x\",\"val\":\"0x%02x\"}",
            i ? "," : "",
            s_oracle_vram_trace.log[idx].frame,
            s_oracle_vram_trace.log[idx].adr_byte,
            s_oracle_vram_trace.log[idx].val);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

#if SNESRECOMP_REVERSE_DEBUG
static void cmd_trace_wram(const char *args) {
    unsigned int lo = 0, hi = 0;
    sscanf(args, "%x %x", &lo, &hi);
    if (hi < lo || hi > 0x1ffff) {
        send_fmt("{\"error\":\"bad range (max \\$1ffff for 128KB WRAM)\"}"); return;
    }
    if (s_wram_trace.nranges >= MAX_WRAM_TRACE_RANGES) {
        send_fmt("{\"error\":\"too many ranges (max %d) — call trace_wram_reset first\"}",
                 MAX_WRAM_TRACE_RANGES); return;
    }
    s_wram_trace.ranges[s_wram_trace.nranges].lo = lo;
    s_wram_trace.ranges[s_wram_trace.nranges].hi = hi;
    s_wram_trace.nranges++;
    s_wram_trace.active = 1;
    send_fmt("{\"ok\":true,\"lo\":\"0x%05x\",\"hi\":\"0x%05x\",\"nranges\":%d}",
             lo, hi, s_wram_trace.nranges);
}

static void cmd_trace_wram_reset(const char *args) {
    (void)args;
    s_wram_trace.nranges = 0;
    s_wram_trace.write_idx = 0;
    s_wram_trace.count = 0;
    s_wram_trace.active = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_get_wram_trace(const char *args) {
    (void)args;
    if (!s_wram_trace.active) {
        send_fmt("{\"error\":\"no wram trace active (or SNESRECOMP_REVERSE_DEBUG=0)\"}"); return;
    }
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf), "{\"ranges\":[");
    for (int i = 0; i < s_wram_trace.nranges; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s[\"0x%05x\",\"0x%05x\"]", i ? "," : "",
            s_wram_trace.ranges[i].lo, s_wram_trace.ranges[i].hi);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"entries\":%d,\"log\":[", s_wram_trace.count);
    int start = s_wram_trace.count < WRAM_TRACE_LOG_SIZE ? 0 :
                s_wram_trace.write_idx - WRAM_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 4096;
    for (int i = 0; i < s_wram_trace.count && pos < budget; i++) {
        int idx = (start + i) % WRAM_TRACE_LOG_SIZE;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"adr\":\"0x%05x\","
            "\"old\":\"0x%04x\",\"val\":\"0x%04x\",\"w\":%u,"
            "\"bi\":%llu,\"func\":\"%s\",\"parent\":\"%s\"}",
            i ? "," : "",
            s_wram_trace.log[idx].frame,
            s_wram_trace.log[idx].adr,
            s_wram_trace.log[idx].old_val,
            s_wram_trace.log[idx].val,
            (unsigned)s_wram_trace.log[idx].width,
            (unsigned long long)s_wram_trace.log[idx].block_idx,
            s_wram_trace.log[idx].func,
            s_wram_trace.log[idx].parent);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

// ---- Tier 2.5 TCP commands ----
static void cmd_break_add(const char *args) {
    uint32_t pc = 0;
    if (!args || sscanf(args, "%x", &pc) != 1) {
        send_fmt("{\"error\":\"usage: break_add <hex_pc>\"}"); return;
    }
    if (s_rdb_break_count >= RDB_MAX_BREAKS) {
        send_fmt("{\"error\":\"break table full (max %d)\"}", RDB_MAX_BREAKS); return;
    }
    s_rdb_break_pcs[s_rdb_break_count++] = pc;
    s_rdb_break_armed = 1;
    send_fmt("{\"ok\":true,\"pc\":\"0x%06x\",\"count\":%d}", pc, s_rdb_break_count);
}

static void cmd_break_clear(const char *args) {
    (void)args;
    s_rdb_break_count = 0;
    s_rdb_break_armed = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_break_list(const char *args) {
    (void)args;
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "{\"count\":%d,\"pcs\":[", s_rdb_break_count);
    for (int i = 0; i < s_rdb_break_count; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%06x\"",
                        i ? "," : "", s_rdb_break_pcs[i]);
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_step_block(const char *args) {
    (void)args;
    // Arm one-shot: pause at the very next block hook (regardless of bp).
    s_rdb_step_pending = 1;
    // If currently parked, unblock so the game runs to the next hook.
    s_paused = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_break_continue(const char *args) {
    (void)args;
    // Resume execution past the current parked block. Breakpoints stay
    // armed; execution will pause again if it hits another break_add'd pc.
    s_paused = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_parked(const char *args) {
    (void)args;
    int watch_hit = (s_rdb_parked_watch_idx >= 0);
    int block_parked = (s_rdb_parked_pc != 0);
    char watch_buf[160] = {0};
    if (watch_hit) {
        snprintf(watch_buf, sizeof(watch_buf),
                 ",\"watch_idx\":%d,\"watch_addr\":\"0x%05x\","
                 "\"watch_val\":\"0x%04x\",\"watch_width\":%u,\"writer\":\"%s\"",
                 s_rdb_parked_watch_idx,
                 s_rdb_parked_watch_addr,
                 s_rdb_parked_watch_val,
                 (unsigned)s_rdb_parked_watch_width,
                 s_rdb_parked_watch_func);
    }
    // Emit stack only when parked on a watch (snapshot was taken then).
    char stack_buf[RDB_PARKED_STACK_DEPTH * 56 + 64] = {0};
    if (watch_hit && s_rdb_parked_stack_depth > 0) {
        int depth = s_rdb_parked_stack_depth;
        int shown = depth < RDB_PARKED_STACK_DEPTH ? depth : RDB_PARKED_STACK_DEPTH;
        int pos = snprintf(stack_buf, sizeof(stack_buf),
                           ",\"stack_depth\":%d,\"stack\":[", depth);
        for (int s = 0; s < shown && pos + 80 < (int)sizeof(stack_buf); s++) {
            pos += snprintf(stack_buf + pos, sizeof(stack_buf) - pos,
                            "%s\"%s\"", s ? "," : "", s_rdb_parked_stack[s]);
        }
        snprintf(stack_buf + pos, sizeof(stack_buf) - pos, "]");
    }
    send_fmt("{\"parked\":%s,\"reason\":\"%s\",\"pc\":\"0x%06x\","
             "\"break_armed\":%d,\"step_pending\":%d,"
             "\"watch_armed\":%d,\"watch_count\":%d%s%s}",
             (block_parked || watch_hit) ? "true" : "false",
             watch_hit ? "watch" : (block_parked ? "break" : "none"),
             s_rdb_parked_pc,
             s_rdb_break_armed,
             s_rdb_step_pending,
             s_rdb_watch_armed,
             s_rdb_watch_count,
             watch_buf,
             stack_buf);
}

// ---- Tier 2.5 WRAM-watchpoint TCP commands ----
// watch_add <hex_addr> [hex_val]
//   Pause when the given WRAM offset is written. If hex_val is provided,
//   only match that exact value. Addresses are 20-bit WRAM offsets
//   (bank $7E = 0x00000..0x0FFFF, bank $7F = 0x10000..0x1FFFF).
static void cmd_watch_add(const char *args) {
    uint32_t addr = 0;
    uint32_t val = 0;
    int n = args ? sscanf(args, "%x %x", &addr, &val) : 0;
    if (n < 1) {
        send_fmt("{\"error\":\"usage: watch_add <hex_addr> [hex_val]\"}"); return;
    }
    if (s_rdb_watch_count >= RDB_MAX_WATCHES) {
        send_fmt("{\"error\":\"watch table full (max %d)\"}", RDB_MAX_WATCHES); return;
    }
    int idx = s_rdb_watch_count;
    s_rdb_watches[idx].addr = addr;
    s_rdb_watches[idx].match_val = (n >= 2) ? (int32_t)val : -1;
    s_rdb_watch_count++;
    s_rdb_watch_armed = 1;
    if (n >= 2)
        send_fmt("{\"ok\":true,\"idx\":%d,\"addr\":\"0x%05x\",\"match_val\":\"0x%04x\",\"count\":%d}",
                 idx, addr, val, s_rdb_watch_count);
    else
        send_fmt("{\"ok\":true,\"idx\":%d,\"addr\":\"0x%05x\",\"match_val\":\"any\",\"count\":%d}",
                 idx, addr, s_rdb_watch_count);
}

static void cmd_watch_clear(const char *args) {
    (void)args;
    s_rdb_watch_count = 0;
    s_rdb_watch_armed = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_watch_list(const char *args) {
    (void)args;
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "{\"count\":%d,\"watches\":[", s_rdb_watch_count);
    for (int i = 0; i < s_rdb_watch_count; i++) {
        int32_t mv = s_rdb_watches[i].match_val;
        if (mv < 0)
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%s{\"idx\":%d,\"addr\":\"0x%05x\",\"match_val\":\"any\"}",
                            i ? "," : "", i, s_rdb_watches[i].addr);
        else
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%s{\"idx\":%d,\"addr\":\"0x%05x\",\"match_val\":\"0x%04x\"}",
                            i ? "," : "", i, s_rdb_watches[i].addr, (uint32_t)mv);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

// watch_continue: alias for break_continue when parked on a watchpoint.
// Kept as a named command so probe scripts read clearly.
static void cmd_watch_continue(const char *args) {
    (void)args;
    s_paused = 0;
    send_fmt("{\"ok\":true}");
}

// ---- Tier 3: time-travel WRAM inspection ----
//
// block_idx_now: current monotonic block counter.
// tier3_anchor_on [interval=4096]: arm periodic full-WRAM snapshots.
// tier3_anchor_off: stop snapshotting; keep existing snapshots.
// tier3_anchor_status: how many anchors held, intervals.
// wram_at_block <block_idx> [start_addr=0] [len=128]: reconstruct
//   historical WRAM by starting from the nearest anchor (or zeros if
//   no anchor) and replaying every Tier 1 wram_trace write up to the
//   target block_idx. Returns the requested slice as hex.
// wram_first_change <hex_addr> [from_block=0] [to_block=current]:
//   scan wram_trace for the first entry that writes addr in the
//   range, returning (block_idx, val, frame, func).
//
// Reconstruction quality requires Tier 1 trace_wram covering the
// queried range. For best results: arm trace_wram on a wide range
// at session start (e.g., 0 1ffff) and tier3_anchor_on so the ring
// is populated.
static void cmd_block_idx_now(const char *args) {
    (void)args;
    send_fmt("{\"ok\":true,\"block_idx\":%llu,\"frame\":%d}",
             (unsigned long long)g_block_counter, snes_frame_counter);
}

static void cmd_tier3_anchor_on(const char *args) {
    unsigned int interval = 4096;
    if (args) sscanf(args, "%u", &interval);
    if (interval < 1) interval = 1;
    s_anchor_interval = interval;
    s_anchor_active = 1;
    send_fmt("{\"ok\":true,\"interval\":%u,\"ring_size\":%d}",
             interval, ANCHOR_RING_SIZE);
}

static void cmd_tier3_anchor_off(const char *args) {
    (void)args;
    s_anchor_active = 0;
    send_fmt("{\"ok\":true,\"count\":%d}", s_anchor_count);
}

static void cmd_tier3_anchor_status(const char *args) {
    (void)args;
    char ranges[1024];
    int pos = snprintf(ranges, sizeof(ranges), "[");
    int start = s_anchor_count < ANCHOR_RING_SIZE ? 0 :
                s_anchor_write_idx - ANCHOR_RING_SIZE;
    for (int i = 0; i < s_anchor_count; i++) {
        int idx = (start + i) % ANCHOR_RING_SIZE;
        pos += snprintf(ranges + pos, sizeof(ranges) - pos,
                        "%s{\"bi\":%llu,\"f\":%d}",
                        i ? "," : "",
                        (unsigned long long)s_wram_anchors[idx].block_idx,
                        s_wram_anchors[idx].frame);
        if (pos > (int)sizeof(ranges) - 64) break;
    }
    snprintf(ranges + pos, sizeof(ranges) - pos, "]");
    send_fmt("{\"ok\":true,\"active\":%d,\"interval\":%u,\"count\":%d,\"anchors\":%s}",
             s_anchor_active, s_anchor_interval, s_anchor_count, ranges);
}

// Find largest anchor with block_idx <= target. Returns -1 if none.
static int anchor_find_le(uint64_t target) {
    int best = -1;
    uint64_t best_bi = 0;
    int start = s_anchor_count < ANCHOR_RING_SIZE ? 0 :
                s_anchor_write_idx - ANCHOR_RING_SIZE;
    for (int i = 0; i < s_anchor_count; i++) {
        int idx = (start + i) % ANCHOR_RING_SIZE;
        uint64_t bi = s_wram_anchors[idx].block_idx;
        if (bi <= target && (best < 0 || bi > best_bi)) {
            best = idx;
            best_bi = bi;
        }
    }
    return best;
}

static void cmd_wram_at_block(const char *args) {
    if (!args || !*args) {
        send_fmt("{\"ok\":false,\"error\":\"usage: wram_at_block <block_idx> [start_addr] [len]\"}");
        return;
    }
    unsigned long long target_ull = 0;
    unsigned int start_addr = 0;
    unsigned int len = 128;
    int n = sscanf(args, "%llu %x %u", &target_ull, &start_addr, &len);
    if (n < 1) {
        send_fmt("{\"ok\":false,\"error\":\"bad args\"}");
        return;
    }
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;
    if (start_addr >= 0x20000) start_addr = 0;
    if (start_addr + len > 0x20000) len = 0x20000 - start_addr;
    uint64_t target = (uint64_t)target_ull;

    // Build full reconstructed WRAM into a static scratch buffer, then
    // slice for response. (128KB scratch isn't huge.)
    static uint8_t scratch[0x20000];
    int anchor_idx = anchor_find_le(target);
    uint64_t replay_start_bi = 0;
    if (anchor_idx >= 0) {
        memcpy(scratch, s_wram_anchors[anchor_idx].wram, 0x20000);
        replay_start_bi = s_wram_anchors[anchor_idx].block_idx;
    } else {
        memset(scratch, 0, 0x20000);
    }

    // Replay all wram_trace writes with block_idx in (replay_start_bi, target].
    // Iterate the ring in chronological (write) order.
    int wstart = s_wram_trace.count < WRAM_TRACE_LOG_SIZE ? 0 :
                 s_wram_trace.write_idx - WRAM_TRACE_LOG_SIZE;
    int applied = 0;
    int oldest_seen_bi = -1;
    int newest_seen_bi = -1;
    for (int i = 0; i < s_wram_trace.count; i++) {
        int idx = (wstart + i) % WRAM_TRACE_LOG_SIZE;
        uint64_t bi = s_wram_trace.log[idx].block_idx;
        if ((int)bi < oldest_seen_bi || oldest_seen_bi < 0) oldest_seen_bi = (int)bi;
        if ((int)bi > newest_seen_bi) newest_seen_bi = (int)bi;
        if (bi <= replay_start_bi) continue;
        if (bi > target) continue;
        uint32_t a = s_wram_trace.log[idx].adr;
        uint16_t v = s_wram_trace.log[idx].val;
        uint8_t  w = s_wram_trace.log[idx].width;
        if (a < 0x20000) {
            scratch[a] = (uint8_t)(v & 0xff);
            if (w == 2 && a + 1 < 0x20000) scratch[a + 1] = (uint8_t)((v >> 8) & 0xff);
        }
        applied++;
    }

    // Hex-encode the requested slice.
    char hex[8192];
    int pos = 0;
    for (unsigned int i = 0; i < len && pos + 3 < (int)sizeof(hex); i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x", scratch[start_addr + i]);
    hex[pos] = 0;

    send_fmt("{\"ok\":true,\"target_bi\":%llu,\"anchor_bi\":%lld,"
             "\"applied_writes\":%d,\"trace_bi_range\":[%d,%d],"
             "\"start\":\"0x%05x\",\"len\":%u,\"hex\":\"%s\"}",
             (unsigned long long)target,
             (anchor_idx >= 0) ? (long long)s_wram_anchors[anchor_idx].block_idx : -1LL,
             applied, oldest_seen_bi, newest_seen_bi,
             start_addr, len, hex);
}

static void cmd_wram_first_change(const char *args) {
    if (!args || !*args) {
        send_fmt("{\"ok\":false,\"error\":\"usage: wram_first_change <hex_addr> [from_block=0] [to_block=current]\"}");
        return;
    }
    unsigned int addr = 0;
    unsigned long long from_bi = 0, to_bi = (unsigned long long)g_block_counter;
    int n = sscanf(args, "%x %llu %llu", &addr, &from_bi, &to_bi);
    if (n < 1) {
        send_fmt("{\"ok\":false,\"error\":\"bad args\"}");
        return;
    }
    int wstart = s_wram_trace.count < WRAM_TRACE_LOG_SIZE ? 0 :
                 s_wram_trace.write_idx - WRAM_TRACE_LOG_SIZE;
    for (int i = 0; i < s_wram_trace.count; i++) {
        int idx = (wstart + i) % WRAM_TRACE_LOG_SIZE;
        uint64_t bi = s_wram_trace.log[idx].block_idx;
        if (bi < from_bi || bi > to_bi) continue;
        uint32_t a = s_wram_trace.log[idx].adr;
        uint8_t  w = s_wram_trace.log[idx].width;
        // Match if the write touches `addr` (covers byte/word straddles).
        if (a == addr || (w == 2 && a + 1 == addr)) {
            send_fmt("{\"ok\":true,\"found\":true,\"bi\":%llu,\"frame\":%d,"
                     "\"adr\":\"0x%05x\",\"val\":\"0x%04x\",\"w\":%u,"
                     "\"func\":\"%s\",\"parent\":\"%s\"}",
                     (unsigned long long)bi,
                     s_wram_trace.log[idx].frame,
                     a, s_wram_trace.log[idx].val,
                     (unsigned)w,
                     s_wram_trace.log[idx].func,
                     s_wram_trace.log[idx].parent);
            return;
        }
    }
    send_fmt("{\"ok\":true,\"found\":false,\"addr\":\"0x%05x\","
             "\"from_bi\":%llu,\"to_bi\":%llu}",
             addr, from_bi, to_bi);
}

// Always-on WRAM trace query: list every recorded write that touches
// `addr` within the given frame window. Probes use this instead of
// `trace_wram` (arm-and-record), so they consume the always-on ring
// without wiping it.
//
// Usage: wram_writes_at <hex_addr> [from_frame=0] [to_frame=current] [limit=64]
static void cmd_wram_writes_at(const char *args) {
    if (!args || !*args) {
        send_fmt("{\"ok\":false,\"error\":\"usage: wram_writes_at <hex_addr> [from_frame=0] [to_frame=current] [limit=64]\"}");
        return;
    }
    unsigned int addr = 0;
    int from_frame = 0;
    int to_frame = INT_MAX;
    int limit = 64;
    int n = sscanf(args, "%x %d %d %d", &addr, &from_frame, &to_frame, &limit);
    if (n < 1) {
        send_fmt("{\"ok\":false,\"error\":\"bad args\"}");
        return;
    }
    if (limit > 4096) limit = 4096;
    int wstart = s_wram_trace.count < WRAM_TRACE_LOG_SIZE ? 0 :
                 s_wram_trace.write_idx - WRAM_TRACE_LOG_SIZE;
    static char buf[1048576];   /* 1MB — accommodates up to 4096 entries
                                   each ~250 bytes JSON */
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"addr\":\"0x%05x\",\"from\":%d,\"to\":%d,\"matches\":[",
        addr, from_frame, to_frame);
    int matched = 0;
    for (int i = 0; i < s_wram_trace.count && matched < limit; i++) {
        int idx = (wstart + i) % WRAM_TRACE_LOG_SIZE;
        uint32_t a = s_wram_trace.log[idx].adr;
        uint8_t  w = s_wram_trace.log[idx].width;
        int f = s_wram_trace.log[idx].frame;
        if (f < from_frame || f > to_frame) continue;
        if (a != addr && !(w == 2 && a + 1 == addr)) continue;
        if (pos > (int)sizeof(buf) - 512) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"adr\":\"0x%05x\","
            "\"old\":\"0x%04x\",\"val\":\"0x%04x\",\"w\":%u,"
            "\"bi\":%llu,\"func\":\"%s\",\"parent\":\"%s\"}",
            matched ? "," : "",
            f, a,
            s_wram_trace.log[idx].old_val,
            s_wram_trace.log[idx].val,
            (unsigned)w,
            (unsigned long long)s_wram_trace.log[idx].block_idx,
            s_wram_trace.log[idx].func,
            s_wram_trace.log[idx].parent);
        matched++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", matched);
    send_line(buf);
}

static void cmd_trace_blocks(const char *args) {
    (void)args;
    s_block_trace.active = 1;
    send_fmt("{\"ok\":true,\"max_entries\":%d}", BLOCK_TRACE_LOG_SIZE);
}

static void cmd_trace_blocks_reset(const char *args) {
    (void)args;
    s_block_trace.active = 0;
    s_block_trace.write_idx = 0;
    s_block_trace.count = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_get_block_trace(const char *args) {
    int from_frame = -1, to_frame = -1;
    uint32_t pc_lo = 0, pc_hi = 0xFFFFFF;
    int pc_filter_set = 0;
    char func_filter[48] = {0};
    // Index-based pagination (relative to the live ring, oldest=0).
    // Distinct from from=/to= which filter by snes_frame_counter range.
    // -1 means "not set"; idx_lim defaults to ring max so legacy callers
    // that don't pass it get the same behavior they did before.
    int idx_from = 0;
    int idx_lim  = BLOCK_TRACE_LOG_SIZE;
    if (args) {
        // Parse idx_* BEFORE the from=/to= scan because strstr("from=")
        // would otherwise match the substring inside "idx_from=".
        const char *p = strstr(args, "idx_from=");
        if (p) sscanf(p + 9, "%d", &idx_from);
        p = strstr(args, "idx_lim=");
        if (p) sscanf(p + 8, "%d", &idx_lim);

        // Frame-range filter (legacy semantics).
        p = strstr(args, "from=");
        // Skip if this 'from=' is actually inside 'idx_from='.
        if (p && (p == args || p[-1] != '_')) sscanf(p + 5, "%d", &from_frame);
        p = strstr(args, "to=");
        if (p) sscanf(p + 3, "%d", &to_frame);
        p = strstr(args, "func=");
        if (p) {
            int i = 0; p += 5;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i < 47)
                func_filter[i++] = *p++;
            func_filter[i] = 0;
        }
        // PC range: pc_lo=0xXXXXXX pc_hi=0xXXXXXX (24-bit bank:pc).
        // Use this when the func attribution is unreliable (recomp stack
        // depth-cap can corrupt g_last_recomp_func after deep call
        // sequences).
        p = strstr(args, "pc_lo=");
        if (p) { sscanf(p + 6, "%x", &pc_lo); pc_filter_set = 1; }
        p = strstr(args, "pc_hi=");
        if (p) { sscanf(p + 6, "%x", &pc_hi); pc_filter_set = 1; }
    }
    if (idx_from < 0) idx_from = 0;
    if (idx_lim  < 1) idx_lim  = 1;
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf), "{\"entries\":%d,\"log\":[", s_block_trace.count);
    int start = s_block_trace.count < BLOCK_TRACE_LOG_SIZE ? 0 :
                s_block_trace.write_idx - BLOCK_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 4096;
    int first = 1;
    int emitted = 0;
    int last_i = idx_from - 1;
    for (int i = idx_from; i < s_block_trace.count && pos < budget && emitted < idx_lim; i++) {
        last_i = i;
        int idx = (start + i) % BLOCK_TRACE_LOG_SIZE;
        int f = s_block_trace.log[idx].frame;
        if (from_frame >= 0 && f < from_frame) continue;
        if (to_frame >= 0 && f > to_frame) continue;
        if (func_filter[0] && !strstr(s_block_trace.log[idx].func, func_filter)) continue;
        if (pc_filter_set) {
            uint32_t pc = s_block_trace.log[idx].pc;
            if (pc < pc_lo || pc > pc_hi) continue;
        }
        // Format register values: hex if known, "?" if RDB_REG_UNKNOWN.
        char a_buf[12], x_buf[12], y_buf[12];
        if (s_block_trace.log[idx].a == 0xFFFFFFFFu) snprintf(a_buf, sizeof(a_buf), "\"?\"");
        else snprintf(a_buf, sizeof(a_buf), "\"0x%04x\"", s_block_trace.log[idx].a & 0xFFFF);
        if (s_block_trace.log[idx].x == 0xFFFFFFFFu) snprintf(x_buf, sizeof(x_buf), "\"?\"");
        else snprintf(x_buf, sizeof(x_buf), "\"0x%04x\"", s_block_trace.log[idx].x & 0xFFFF);
        if (s_block_trace.log[idx].y == 0xFFFFFFFFu) snprintf(y_buf, sizeof(y_buf), "\"?\"");
        else snprintf(y_buf, sizeof(y_buf), "\"0x%04x\"", s_block_trace.log[idx].y & 0xFFFF);

        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"d\":%d,\"pc\":\"0x%06x\","
            "\"a\":%s,\"x\":%s,\"y\":%s,\"bi\":%llu,\"func\":\"%s\"}",
            first ? "" : ",",
            f,
            s_block_trace.log[idx].depth,
            s_block_trace.log[idx].pc,
            a_buf, x_buf, y_buf,
            (unsigned long long)s_block_trace.log[idx].block_idx,
            s_block_trace.log[idx].func);
        first = 0;
        emitted++;
    }
    // next_idx is the resume cursor for the next idx_from= page; equals
    // last_i+1 so the caller can paginate without inferring it client-side.
    snprintf(buf + pos, sizeof(buf) - pos,
             "],\"emitted\":%d,\"next_idx\":%d,\"total\":%d}",
             emitted, last_i + 1, s_block_trace.count);
    send_line(buf);
}

static void cmd_trace_calls(const char *args) {
    (void)args;
    s_call_trace.active = 1;
    send_fmt("{\"ok\":true,\"max_entries\":%d}", CALL_TRACE_LOG_SIZE);
}

static void cmd_trace_calls_reset(const char *args) {
    (void)args;
    s_call_trace.active = 0;
    s_call_trace.write_idx = 0;
    s_call_trace.count = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_get_call_trace(const char *args) {
    // Optional filters: `from=N to=M` (frame range), `max_depth=D`
    // (skip pushes deeper than D — useful to drop Decompress / nested-
    // recursion noise), `contains=SUBSTR` (only emit entries where
    // func or parent contains SUBSTR — case-sensitive).
    int from_frame = -1, to_frame = -1, max_depth = -1;
    char contains[48] = {0};
    if (args) {
        const char *p = strstr(args, "from=");
        if (p) sscanf(p + 5, "%d", &from_frame);
        p = strstr(args, "to=");
        if (p) sscanf(p + 3, "%d", &to_frame);
        p = strstr(args, "max_depth=");
        if (p) sscanf(p + 10, "%d", &max_depth);
        p = strstr(args, "contains=");
        if (p) {
            int i = 0;
            p += 9;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i < 47) {
                contains[i++] = *p++;
            }
            contains[i] = 0;
        }
    }
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf), "{\"entries\":%d,\"log\":[", s_call_trace.count);
    int start = s_call_trace.count < CALL_TRACE_LOG_SIZE ? 0 :
                s_call_trace.write_idx - CALL_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 4096;
    int first = 1;
    int emitted = 0;
    for (int i = 0; i < s_call_trace.count && pos < budget; i++) {
        int idx = (start + i) % CALL_TRACE_LOG_SIZE;
        int f = s_call_trace.log[idx].frame;
        if (from_frame >= 0 && f < from_frame) continue;
        if (to_frame >= 0 && f > to_frame) continue;
        if (max_depth >= 0 && s_call_trace.log[idx].depth > max_depth) continue;
        if (contains[0]) {
            if (!strstr(s_call_trace.log[idx].func, contains)
                && !strstr(s_call_trace.log[idx].parent, contains)) continue;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"d\":%d,\"func\":\"%s\",\"parent\":\"%s\"}",
            first ? "" : ",",
            f,
            s_call_trace.log[idx].depth,
            s_call_trace.log[idx].func,
            s_call_trace.log[idx].parent);
        first = 0;
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", emitted);
    send_line(buf);
}
#endif

static void cmd_get_reg_trace(const char *args) {
    if (!s_reg_trace.active) {
        send_fmt("{\"error\":\"no reg trace active\"}"); return;
    }
    int nostack = args && strstr(args, "nostack") != NULL;
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf), "{\"ranges\":[");
    for (int i = 0; i < s_reg_trace.nranges; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s[\"0x%04x\",\"0x%04x\"]", i ? "," : "",
            s_reg_trace.ranges[i].lo, s_reg_trace.ranges[i].hi);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"entries\":%d,\"log\":[", s_reg_trace.count);
    int start = s_reg_trace.count < REG_TRACE_LOG_SIZE ? 0 :
                s_reg_trace.write_idx - REG_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 4096;
    for (int i = 0; i < s_reg_trace.count && pos < budget; i++) {
        int idx = (start + i) % REG_TRACE_LOG_SIZE;
        if (nostack) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"f\":%d,\"adr\":\"0x%04x\",\"val\":\"0x%02x\",\"func\":\"%s\"}",
                i ? "," : "",
                s_reg_trace.log[idx].frame,
                s_reg_trace.log[idx].adr,
                s_reg_trace.log[idx].val,
                s_reg_trace.log[idx].func);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"f\":%d,\"adr\":\"0x%04x\",\"val\":\"0x%02x\",\"func\":\"%s\",\"stack\":[",
                i ? "," : "",
                s_reg_trace.log[idx].frame,
                s_reg_trace.log[idx].adr,
                s_reg_trace.log[idx].val,
                s_reg_trace.log[idx].func);
            for (int s = 0; s < s_reg_trace.log[idx].stack_depth; s++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s\"%s\"", s ? "," : "",
                    s_reg_trace.log[idx].stack[s] ? s_reg_trace.log[idx].stack[s] : "?");
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
        }
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_trace_range(const char *args) {
    unsigned int base = 0;
    unsigned int len = 0;
    sscanf(args, "%x %x", &base, &len);
    if (len == 0 || len > RANGE_TRACE_MAX) {
        send_fmt("{\"error\":\"len must be 1..%d\"}", RANGE_TRACE_MAX);
        return;
    }
    s_range_trace.base = base;
    s_range_trace.len = (int)len;
    s_range_trace.write_idx = 0;
    s_range_trace.count = 0;
    if (s_ram) {
        for (int i = 0; i < (int)len; i++)
            s_range_trace.prev_val[i] = s_ram[base + i];
    } else {
        for (int i = 0; i < (int)len; i++) s_range_trace.prev_val[i] = 0;
    }
    s_range_trace.active = 1;
    send_fmt("{\"ok\":true,\"tracing_range\":\"0x%x\",\"len\":%u}", base, len);
}

static void cmd_get_trace_range(const char *args) {
    if (!s_range_trace.active) {
        send_fmt("{\"error\":\"no range trace active\"}");
        return;
    }
    static char buf[262144];
    int pos = snprintf(buf, sizeof(buf),
        "{\"base\":\"0x%x\",\"len\":%d,\"entries\":%d,\"log\":[",
        s_range_trace.base, s_range_trace.len, s_range_trace.count);
    int start = s_range_trace.count < RANGE_TRACE_LOG_SIZE ? 0 :
                s_range_trace.write_idx - RANGE_TRACE_LOG_SIZE;
    for (int i = 0; i < s_range_trace.count && pos < 250000; i++) {
        int idx = (start + i) % RANGE_TRACE_LOG_SIZE;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"off\":%u,\"old\":\"0x%02x\",\"new\":\"0x%02x\",\"func\":\"%s\",\"stack\":[",
            i ? "," : "",
            s_range_trace.log[idx].frame,
            (unsigned)s_range_trace.log[idx].offset,
            s_range_trace.log[idx].old_val,
            s_range_trace.log[idx].new_val,
            s_range_trace.log[idx].func);
        for (int s = 0; s < s_range_trace.log[idx].stack_depth; s++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", s ? "," : "",
                s_range_trace.log[idx].stack[s] ? s_range_trace.log[idx].stack[s] : "?");
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_loadstate(const char *args) {
    int slot = 0;
    if (args[0]) sscanf(args, "%d", &slot);
    if (slot < 0 || slot > 9) {
        send_fmt("{\"error\":\"slot must be 0-9\"}");
        return;
    }
    s_pending_loadstate = slot;
    send_fmt("{\"ok\":true,\"loading_slot\":%d}", slot);
}

// ---- L3 harness: synchronous save_state / load_state ----
// Minimal snapshot — serializes full SNES state (CPU/PPU/DMA/APU/cart/WRAM)
// via snes_saveload to a raw binary file. No replay log, no state-recorder
// overhead. Intended for per-function L3 tests: capture a fixture, replay
// into both recomp and oracle, invoke one function, diff.

typedef struct FileSli {
    SaveLoadInfo sli;
    FILE *f;
    int is_save;
    int error;
    size_t total;
} FileSli;

static void _file_sli_func(SaveLoadInfo *info, void *data, size_t size) {
    FileSli *fs = (FileSli *)info;
    if (fs->error) return;
    size_t got;
    if (fs->is_save)
        got = fwrite(data, 1, size, fs->f);
    else
        got = fread(data, 1, size, fs->f);
    if (got != size) fs->error = 1;
    fs->total += size;
}

// 4-byte magic + 4-byte version lets us evolve the format.
#define L3_SNAP_MAGIC 0x4c33534e  /* "L3SN" */
#define L3_SNAP_VERSION 1

static void cmd_save_state(const char *args) {
    char filename[512];
    if (sscanf(args, "%500s", filename) != 1) {
        send_fmt("{\"error\":\"usage: save_state <filename>\"}");
        return;
    }
    FILE *f = fopen(filename, "wb");
    if (!f) {
        send_fmt("{\"error\":\"fopen failed: %s\"}", filename);
        return;
    }
    uint32_t magic = L3_SNAP_MAGIC, version = L3_SNAP_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    FileSli fs = {{_file_sli_func}, f, 1, 0, 0};
    snes_saveload(g_snes, &fs.sli);
    fclose(f);
    if (fs.error) {
        send_fmt("{\"error\":\"write failed after %zu bytes\"}", fs.total);
        return;
    }
    send_fmt("{\"ok\":true,\"bytes\":%zu,\"file\":\"%s\"}", fs.total + 8, filename);
}

// ---- L3 harness: minimal-input fixture helpers ----
// write_ram / zero_ram / set_cpu — building blocks for the input-injection
// style of per-function test. Each test zeros both runtimes, writes its
// small set of input bytes, sets CPU regs, invokes, then reads the
// declared output regions. No savestate needed.

static int _hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static void cmd_write_ram(const char *args) {
    unsigned int addr = 0;
    if (sscanf(args, "%x", &addr) != 1) {
        send_fmt("{\"error\":\"usage: write_ram <addr_hex> <hex_bytes>\"}");
        return;
    }
    // Skip past addr to hex blob.
    const char *p = args;
    while (*p && !((*p == ' ') || (*p == '\t'))) p++;
    while (*p == ' ' || *p == '\t') p++;
    int count = 0;
    while (p[0] && p[1] && addr + count < 0x20000) {
        int hi = _hex_nibble(p[0]);
        int lo = _hex_nibble(p[1]);
        if (hi < 0 || lo < 0) break;
        g_ram[addr + count] = (uint8_t)((hi << 4) | lo);
        count++;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }
    send_fmt("{\"ok\":true,\"addr\":\"0x%x\",\"count\":%d}", addr, count);
}

static void cmd_zero_ram(const char *args) {
    (void)args;
    memset(g_ram, 0, 0x20000);
    send_fmt("{\"ok\":true,\"size\":%u}", 0x20000);
}

// set_cpu key=val key=val ...   where key in {a,x,y,sp,dp,db,pb,pc,p,e}
// Values are parsed as hex (prefix optional).
static void cmd_set_cpu(const char *args) {
    const char *p = args;
    int count = 0;
    char keybuf[16];
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int klen = 0;
        while (p[klen] && p[klen] != '=' && klen < 15) { keybuf[klen] = p[klen]; klen++; }
        keybuf[klen] = 0;
        if (p[klen] != '=') break;
        p += klen + 1;
        unsigned int val = 0;
        int nread = 0;
        if (sscanf(p, "%x%n", &val, &nread) != 1) break;
        p += nread;
        if      (strcmp(keybuf, "a")  == 0) g_snes_cpu->a  = (uint16_t)val;
        else if (strcmp(keybuf, "x")  == 0) g_snes_cpu->x  = (uint16_t)val;
        else if (strcmp(keybuf, "y")  == 0) g_snes_cpu->y  = (uint16_t)val;
        else if (strcmp(keybuf, "sp") == 0) g_snes_cpu->sp = (uint16_t)val;
        else if (strcmp(keybuf, "dp") == 0) g_snes_cpu->dp = (uint16_t)val;
        else if (strcmp(keybuf, "db") == 0) g_snes_cpu->db = (uint8_t)val;
        else if (strcmp(keybuf, "pb") == 0) g_snes_cpu->k  = (uint8_t)val;
        else if (strcmp(keybuf, "pc") == 0) g_snes_cpu->pc = (uint16_t)val;
        else if (strcmp(keybuf, "p")  == 0) cpu_setFlags(g_snes_cpu, (uint8_t)val);
        else if (strcmp(keybuf, "e")  == 0) g_snes_cpu->e  = (val != 0);
        else { send_fmt("{\"error\":\"unknown cpu field: %s\"}", keybuf); return; }
        count++;
    }
    send_fmt("{\"ok\":true,\"fields_set\":%d}", count);
}

// ---- L3 harness: invoke one recompiled function by name ----
// v1 ABI dispatch (void() / void(uint8)) — disabled in v2 builds where
// every recompiled function takes CpuState*. A v2 registry will replace
// this in a follow-up.
static void cmd_invoke_recomp(const char *args) {
    (void)args;
    send_fmt("{\"error\":\"invoke_recomp disabled in v2 build\"}");
}

static void cmd_load_state(const char *args) {
    char filename[512];
    if (sscanf(args, "%500s", filename) != 1) {
        send_fmt("{\"error\":\"usage: load_state <filename>\"}");
        return;
    }
    FILE *f = fopen(filename, "rb");
    if (!f) {
        send_fmt("{\"error\":\"fopen failed: %s\"}", filename);
        return;
    }
    uint32_t magic = 0, version = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != L3_SNAP_MAGIC) {
        fclose(f);
        send_fmt("{\"error\":\"bad magic: expected L3 snapshot\"}");
        return;
    }
    if (fread(&version, 4, 1, f) != 1 || version != L3_SNAP_VERSION) {
        fclose(f);
        send_fmt("{\"error\":\"bad version: got %u want %u\"}", version, L3_SNAP_VERSION);
        return;
    }
    FileSli fs = {{_file_sli_func}, f, 0, 0, 0};
    snes_saveload(g_snes, &fs.sli);
    fclose(f);
    if (fs.error) {
        send_fmt("{\"error\":\"read failed after %zu bytes\"}", fs.total);
        return;
    }
    send_fmt("{\"ok\":true,\"bytes\":%zu,\"file\":\"%s\"}", fs.total + 8, filename);
}

static void cmd_get_frame(const char *args) {
    int frame_num = 0;
    sscanf(args, "%d", &frame_num);
    FrameRecord *r = find_frame(frame_num);
    if (!r) {
        send_fmt("{\"error\":\"frame %d not in buffer (oldest=%d, newest=%d)\"}",
                 frame_num,
                 s_history_count > 0 ? s_frame_history[(s_history_write_idx - s_history_count + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number : -1,
                 s_history_count > 0 ? s_frame_history[(s_history_write_idx - 1 + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number : -1);
        return;
    }
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf),
        "{\"frame\":%d,\"func\":\"%s\"",
        r->frame_number, r->last_func);
    // Add game state snapshot
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        ",\"game_mode\":\"0x%02x\",\"gfx_files\":\"%02x %02x %02x %02x %02x %02x %02x %02x\",\"snap\":\"",
        r->snap[21], r->snap[22], r->snap[23], r->snap[24], r->snap[25],
        r->snap[26], r->snap[27], r->snap[28], r->snap[29]);
    for (int si = 0; si < SNAP_BYTES && pos < (int)sizeof(buf) - 10; si++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%02x", si ? " " : "", r->snap[si]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"");
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
}

static void cmd_frame_range(const char *args) {
    int start = 0, end = 0;
    sscanf(args, "%d %d", &start, &end);
    if (end - start > 500) end = start + 500;
    char buf[32768];
    int pos = snprintf(buf, sizeof(buf), "{\"frames\":[");
    for (int f = start; f <= end && pos < 30000; f++) {
        FrameRecord *r = find_frame(f);
        if (!r) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,"
            "\"mode\":\"0x%02x\",\"gfx\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}",
            pos > 12 ? "," : "",
            r->frame_number,
            r->snap[21],
            r->snap[22], r->snap[23], r->snap[24], r->snap[25],
            r->snap[26], r->snap[27], r->snap[28], r->snap[29]);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_history_status(const char *args) {
    int oldest = s_history_count > 0
        ? s_frame_history[(s_history_write_idx - s_history_count + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number
        : -1;
    int newest = s_history_count > 0
        ? s_frame_history[(s_history_write_idx - 1 + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number
        : -1;
    send_fmt("{\"history\":{\"count\":%d,\"capacity\":%d,\"oldest\":%d,\"newest\":%d}}",
             s_history_count, FRAME_HISTORY_SIZE, oldest, newest);
}

static void cmd_wram_timeseries(const char *args) {
    unsigned int addr = 0, len = 1;
    int from_frame = INT_MIN;
    int to_frame = INT_MIN;
    int limit = 512;
    if (!args || sscanf(args, "%x %u %d %d %d",
                        &addr, &len, &from_frame, &to_frame, &limit) < 1) {
        send_fmt("{\"ok\":false,\"error\":\"usage: wram_timeseries <hex_addr> [len] [from_frame] [to_frame] [limit]\"}");
        return;
    }
    if (len < 1) len = 1;
    if (len > 32) len = 32;
    if (addr + len > 0x20000) {
        send_fmt("{\"ok\":false,\"error\":\"range out of WRAM\"}");
        return;
    }
    if (limit < 1) limit = 1;
    if (limit > 4096) limit = 4096;

    lock_mutex();
    int oldest = s_history_count > 0
        ? s_frame_history[(s_history_write_idx - s_history_count + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number
        : -1;
    int newest = s_history_count > 0
        ? s_frame_history[(s_history_write_idx - 1 + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number
        : -1;
    if (from_frame == INT_MIN) from_frame = oldest;
    if (to_frame == INT_MIN) to_frame = newest;

    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"addr\":\"0x%05x\",\"len\":%u,\"from\":%d,\"to\":%d,\"entries\":[",
        addr, len, from_frame, to_frame);
    uint8_t last[32] = {0};
    int have_last = 0;
    int emitted = 0;
    int considered = 0;
    int truncated = 0;
    int start = (s_history_write_idx - s_history_count + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE;
    for (int i = 0; i < s_history_count; i++) {
        FrameRecord *r = &s_frame_history[(start + i) % FRAME_HISTORY_SIZE];
        if (r->frame_number < from_frame || r->frame_number > to_frame) continue;
        considered++;
        if (have_last && memcmp(last, r->wram + addr, len) == 0) continue;
        if (emitted >= limit || pos > (int)sizeof(buf) - 256) {
            truncated = 1;
            break;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"f\":%d,\"hex\":\"",
                        emitted ? "," : "", r->frame_number);
        for (unsigned int b = 0; b < len && pos < (int)sizeof(buf) - 16; b++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x",
                            r->wram[addr + b]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"}");
        memcpy(last, r->wram + addr, len);
        have_last = 1;
        emitted++;
    }
    unlock_mutex();
    snprintf(buf + pos, sizeof(buf) - pos,
             "],\"emitted\":%d,\"considered\":%d,\"truncated\":%s}",
             emitted, considered, truncated ? "true" : "false");
    send_line(buf);
}

static uint8_t sprite_field(const uint8_t *wram, int base, int slot) {
    return wram[base + slot];
}

static int sprite_timeseries_append(char *buf, int pos, int cap,
                                    const uint8_t *wram, int frame, int slot,
                                    int first) {
    int x = sprite_field(wram, 0x00e4, slot)
          | (sprite_field(wram, 0x14e0, slot) << 8);
    int y = sprite_field(wram, 0x00d8, slot)
          | (sprite_field(wram, 0x14d4, slot) << 8);
    return pos + snprintf(buf + pos, cap - pos,
        "%s{\"f\":%d,\"st\":\"%02x\",\"n\":\"%02x\","
        "\"x\":\"%04x\",\"y\":\"%04x\",\"xs\":\"%02x\",\"ys\":\"%02x\","
        "\"bl\":\"%02x\",\"sl\":\"%02x\",\"dir\":\"%02x\","
        "\"t1540\":\"%02x\",\"t1558\":\"%02x\",\"m1fe2\":\"%02x\","
        "\"noobj\":\"%02x\"}",
        first ? "" : ",",
        frame,
        sprite_field(wram, 0x14c8, slot),
        sprite_field(wram, 0x009e, slot),
        x & 0xffff,
        y & 0xffff,
        sprite_field(wram, 0x00b6, slot),
        sprite_field(wram, 0x00aa, slot),
        sprite_field(wram, 0x1588, slot),
        sprite_field(wram, 0x15b8, slot),
        sprite_field(wram, 0x157c, slot),
        sprite_field(wram, 0x1540, slot),
        sprite_field(wram, 0x1558, slot),
        sprite_field(wram, 0x1fe2, slot),
        sprite_field(wram, 0x15dc, slot));
}

static int sprite_timeseries_same(const uint8_t *a, const uint8_t *b, int slot) {
    static const int bases[] = {
        0x14c8, 0x009e, 0x00e4, 0x14e0, 0x00d8, 0x14d4, 0x00b6,
        0x00aa, 0x1588, 0x15b8, 0x157c, 0x1540, 0x1558, 0x1fe2,
        0x15dc
    };
    for (int i = 0; i < (int)(sizeof(bases) / sizeof(bases[0])); i++) {
        int off = bases[i] + slot;
        if (a[off] != b[off]) return 0;
    }
    return 1;
}

static void cmd_sprite_timeseries(const char *args) {
    int slot = 9;
    int from_frame = INT_MIN;
    int to_frame = INT_MIN;
    int changes_only = 1;
    int limit = 512;
    if (args && *args) {
        int n = sscanf(args, "%d %d %d %d %d",
                       &slot, &from_frame, &to_frame, &changes_only, &limit);
        if (n < 1) slot = 9;
    }
    if (slot < 0 || slot >= 12) {
        send_fmt("{\"ok\":false,\"error\":\"slot out of range\",\"slot\":%d}", slot);
        return;
    }
    if (limit < 1) limit = 1;
    if (limit > 4096) limit = 4096;

    lock_mutex();
    int oldest = s_history_count > 0
        ? s_frame_history[(s_history_write_idx - s_history_count + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number
        : -1;
    int newest = s_history_count > 0
        ? s_frame_history[(s_history_write_idx - 1 + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE].frame_number
        : -1;
    if (from_frame == INT_MIN) from_frame = oldest;
    if (to_frame == INT_MIN) to_frame = newest;
    if (from_frame < oldest) from_frame = oldest;
    if (to_frame > newest) to_frame = newest;

    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"slot\":%d,\"from\":%d,\"to\":%d,"
        "\"changes_only\":%s,\"entries\":[",
        slot, from_frame, to_frame, changes_only ? "true" : "false");
    uint8_t last[0x2000];
    int have_last = 0;
    int emitted = 0;
    int considered = 0;
    int truncated = 0;
    for (int f = from_frame; f <= to_frame; f++) {
        FrameRecord *r = find_frame(f);
        if (!r) continue;
        considered++;
        int same = have_last && sprite_timeseries_same(last, r->wram, slot);
        if (!changes_only || !same) {
            if (emitted >= limit || pos > (int)sizeof(buf) - 1024) {
                truncated = 1;
                break;
            }
            pos = sprite_timeseries_append(buf, pos, (int)sizeof(buf),
                                           r->wram, f, slot, emitted == 0);
            emitted++;
        }
        memcpy(last, r->wram, sizeof(last));
        have_last = 1;
    }
    unlock_mutex();
    snprintf(buf + pos, sizeof(buf) - pos,
             "],\"emitted\":%d,\"considered\":%d,\"truncated\":%s}",
             emitted, considered, truncated ? "true" : "false");
    send_line(buf);
}

static void cmd_profile_on(const char *args) {
    s_profile_enabled = 1;
    s_profile_count = 0;
    send_fmt("{\"profile\":\"enabled\"}");
}

static void cmd_profile_off(const char *args) {
    s_profile_enabled = 0;
    send_fmt("{\"profile\":\"disabled\"}");
}

static void cmd_profile_query(const char *args) {
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf),
        "{\"frame_ms\":%.1f,\"frame_num\":%d,\"latched\":%s,\"funcs\":%d,\"top\":[",
        s_profile_frame_ms, s_profile_frame_num,
        s_profile_latched ? "true" : "false", s_profile_count);
    // Sort by call count (simple selection of top 20)
    int used[PROFILE_MAX_FUNCS] = {0};
    for (int t = 0; t < 20 && t < s_profile_count && pos < 7500; t++) {
        int best = -1;
        for (int i = 0; i < s_profile_count; i++) {
            if (!used[i] && (best < 0 || s_profile[i].call_count > s_profile[best].call_count))
                best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s{\"name\":\"%s\",\"calls\":%d}",
                        t ? "," : "", s_profile[best].name, s_profile[best].call_count);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
    // Auto-unlatch after reading so profiling resumes
    if (s_profile_latched) s_profile_latched = 0;
}

static void cmd_latches(const char *args) {
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"count\":%d,\"latches\":[", s_latch_count);
    for (int i = 0; i < s_latch_count && pos < 7000; i++) {
        int idx = (s_latch_write - s_latch_count + i + LATCH_RING_SIZE) % LATCH_RING_SIZE;
        LatchedProfile *lp = &s_latches[idx];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"frame\":%d,\"ms\":%.0f,\"funcs\":%d,\"top\":[",
            i ? "," : "", lp->frame_num, lp->frame_ms, lp->func_count);
        for (int t = 0; t < lp->top_count && pos < 7500; t++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s{\"n\":\"%s\",\"c\":%d}",
                            t ? "," : "", lp->top[t].name, lp->top[t].call_count);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

// ---- get_functions: return all unique function names seen ----
static void cmd_get_functions(const char *args) {
    (void)args;
    // Send as JSON array
    char buf[65536];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"frame\":%d,\"count\":%d,\"functions\":[",
                    snes_frame_counter, s_func_tracker_count);
    for (int i = 0; i < s_func_tracker_count && pos < (int)sizeof(buf) - 200; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", s_func_tracker[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

// ---- Command dispatch ----

// ---- Exhaustive state dump commands ----

static void send_hex_blob(const uint8_t *data, unsigned int len) {
    // Send raw hex bytes in chunks (caller handles JSON wrapper)
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x", data[i]);
        send(s_client_sock, chunk, pos, 0);
    }
}

static void cmd_dump_vram(const char *args) {
    if (!g_ppu) { send_fmt("{\"error\":\"ppu not available\"}"); return; }
    unsigned int addr = 0, len = 65536;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 65536) len = 65536;
    const uint8_t *vram_bytes = (const uint8_t *)g_ppu->vram;
    if (addr + len > 65536) { send_fmt("{\"error\":\"out of range\"}"); return; }
    if (s_client_sock == SOCKET_INVALID) return;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"", addr, len);
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    send_hex_blob(vram_bytes + addr, len);
    send(s_client_sock, "\"}\n", 3, 0);
}

// Historical VRAM dump: reads the ring-buffer snapshot for a specific
// frame. Args: `<frame> [addr_hex] [len]`. If frame isn't in the ring
// (not yet recorded, or evicted), returns an error.
static void cmd_dump_frame_vram(const char *args) {
    int frame_num = -1;
    unsigned int addr = 0, len = 0x10000;
    if (sscanf(args, "%d %x %u", &frame_num, &addr, &len) < 1) {
        send_fmt("{\"error\":\"usage: dump_frame_vram <frame> [addr_hex] [len]\"}");
        return;
    }
    if (len > 0x10000) len = 0x10000;
    if (addr + len > 0x10000) {
        send_fmt("{\"error\":\"out of range\"}");
        return;
    }
    lock_mutex();
    FrameRecord *r = find_frame(frame_num);
    if (!r) {
        unlock_mutex();
        send_fmt("{\"error\":\"frame %d not in ring buffer\"}", frame_num);
        return;
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr),
             "{\"frame\":%d,\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"",
             frame_num, addr, len);
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    // Copy out of the locked record so we don't hold the mutex during send.
    static uint8_t tmp[0x10000];
    memcpy(tmp, r->vram + addr, len);
    unlock_mutex();
    send_hex_blob(tmp, len);
    send(s_client_sock, "\"}\n", 3, 0);
}

// Historical WRAM dump: reads the ring-buffer snapshot for a specific
// frame. Args: `<frame> [addr_hex] [len]`.
static void cmd_dump_frame_wram(const char *args) {
    int frame_num = -1;
    unsigned int addr = 0, len = 0x20000;
    if (sscanf(args, "%d %x %u", &frame_num, &addr, &len) < 1) {
        send_fmt("{\"error\":\"usage: dump_frame_wram <frame> [addr_hex] [len]\"}");
        return;
    }
    if (len > 0x20000) len = 0x20000;
    if (addr + len > 0x20000) {
        send_fmt("{\"error\":\"out of range\"}");
        return;
    }
    lock_mutex();
    FrameRecord *r = find_frame(frame_num);
    if (!r) {
        unlock_mutex();
        send_fmt("{\"error\":\"frame %d not in ring buffer\"}", frame_num);
        return;
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr),
             "{\"frame\":%d,\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"",
             frame_num, addr, len);
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    static uint8_t tmp[0x20000];
    memcpy(tmp, r->wram + addr, len);
    unlock_mutex();
    send_hex_blob(tmp, len);
    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_dump_cgram(const char *args) {
    if (!g_ppu) { send_fmt("{\"error\":\"ppu not available\"}"); return; }
    const uint8_t *cgram_bytes = (const uint8_t *)g_ppu->cgram;
    if (s_client_sock == SOCKET_INVALID) return;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "{\"len\":512,\"hex\":\"");
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    send_hex_blob(cgram_bytes, 512);
    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_dump_oam(const char *args) {
    if (!g_ppu) { send_fmt("{\"error\":\"ppu not available\"}"); return; }
    if (s_client_sock == SOCKET_INVALID) return;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "{\"len\":544,\"hex\":\"");
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    send_hex_blob((const uint8_t *)g_ppu->oam, 512);
    send_hex_blob(g_ppu->highOam, 32);
    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_screenshot(const char *args) {
    if (!g_ppu) { send_fmt("{\"error\":\"ppu not available\"}"); return; }

    // Render current PPU state into a temp buffer. PpuBeginDrawing rebinds
    // ppu->renderBuffer + renderPitch; if we don't restore them, the next
    // SmwDrawPpuFrame from the main loop renders into scr_pixels instead of
    // g_my_pixels, and the on-screen window freezes on whatever was in
    // g_my_pixels at the moment of the first screenshot.
    static uint8_t scr_pixels[256 * 4 * 240];
    uint8_t *saved_render_buffer = g_ppu->renderBuffer;
    uint32_t saved_render_pitch  = g_ppu->renderPitch;
    PpuBeginDrawing(g_ppu, scr_pixels, 256 * 4, 0);

    // Run HDMA + scanlines like SmwDrawPpuFrame but without IRQ
    for (int i = 0; i <= 224; i++)
        ppu_runLine(g_ppu, i);

    // Restore the main loop's binding so the visible window keeps updating.
    g_ppu->renderBuffer = saved_render_buffer;
    g_ppu->renderPitch  = saved_render_pitch;

    // Determine output path
    const char *path = args[0] ? args : "debug_screenshot.bmp";

    // Write 24-bit BMP (no alpha)
    FILE *f = fopen(path, "wb");
    if (!f) { send_fmt("{\"error\":\"cannot open file\",\"path\":\"%s\"}", path); return; }

    int w = 256, h = 224;
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;
    int img_size = stride * h;
    int file_size = 54 + img_size;

    // BMP header
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size; hdr[3] = file_size >> 8; hdr[4] = file_size >> 16; hdr[5] = file_size >> 24;
    hdr[10] = 54; // pixel data offset
    hdr[14] = 40; // DIB header size
    hdr[18] = w; hdr[19] = w >> 8;
    // BMP stores height as negative for top-down
    int neg_h = -h;
    memcpy(&hdr[22], &neg_h, 4);
    hdr[26] = 1; // planes
    hdr[28] = 24; // bpp
    hdr[34] = img_size; hdr[35] = img_size >> 8; hdr[36] = img_size >> 16; hdr[37] = img_size >> 24;

    fwrite(hdr, 1, 54, f);

    // Write pixels (BGRA -> BGR, top to bottom)
    uint8_t row_buf[256 * 3 + 4];
    memset(row_buf, 0, sizeof(row_buf));
    for (int y = 0; y < h; y++) {
        const uint8_t *src = scr_pixels + y * 256 * 4;
        for (int x = 0; x < w; x++) {
            row_buf[x * 3 + 0] = src[x * 4 + 0]; // B
            row_buf[x * 3 + 1] = src[x * 4 + 1]; // G
            row_buf[x * 3 + 2] = src[x * 4 + 2]; // R
        }
        fwrite(row_buf, 1, stride, f);
    }
    fclose(f);

    send_fmt("{\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"frame\":%d}",
             path, w, h, snes_frame_counter);
}

static void cmd_get_ppu_state(const char *args) {
    if (!g_ppu) { send_fmt("{\"error\":\"ppu not available\"}"); return; }
    Ppu *p = g_ppu;
    send_fmt("{\"inidisp\":\"0x%02x\",\"bgmode\":%d,\"mosaic\":\"0x%02x\",\"obsel\":\"0x%02x\","
             "\"setini\":\"0x%02x\","
             "\"bgXsc\":[\"0x%02x\",\"0x%02x\",\"0x%02x\",\"0x%02x\"],"
             "\"bgTileAdr\":\"0x%04x\","
             "\"hScroll\":[%d,%d,%d,%d],\"vScroll\":[%d,%d,%d,%d],"
             "\"screenEnabled\":[\"0x%02x\",\"0x%02x\"],\"screenWindowed\":[\"0x%02x\",\"0x%02x\"],"
             "\"cgadsub\":\"0x%02x\",\"cgwsel\":\"0x%02x\","
             "\"fixedColor\":\"0x%04x\","
             "\"vramPointer\":\"0x%04x\",\"vramIncrement\":%d,\"vramRemapMode\":%d,"
             "\"cgramPointer\":\"0x%02x\","
             "\"window1left\":%d,\"window1right\":%d,\"window2left\":%d,\"window2right\":%d,"
             "\"evenFrame\":%s}",
             p->inidisp, p->bgmode & 7, p->mosaic, p->obsel,
             p->setini,
             p->bgXsc[0], p->bgXsc[1], p->bgXsc[2], p->bgXsc[3],
             p->bgTileAdr,
             p->hScroll[0], p->hScroll[1], p->hScroll[2], p->hScroll[3],
             p->vScroll[0], p->vScroll[1], p->vScroll[2], p->vScroll[3],
             p->screenEnabled[0], p->screenEnabled[1], p->screenWindowed[0], p->screenWindowed[1],
             p->cgadsub, p->cgwsel,
             p->fixedColor,
             p->vramPointer, p->vramIncrement, p->vramRemapMode,
             p->cgramPointer,
             p->window1left, p->window1right, p->window2left, p->window2right,
             p->evenFrame ? "true" : "false");
}

// Interrupt / timing state. Exposes the SNES-level fields that are
// load-bearing for NMI/IRQ timing analysis (inNmi, inVblank, scanline,
// IRQ enables, auto-joypad timer). These were invisible to the debugger
// after the interpreter rip; the Cpu struct's nmiWanted/irqWanted were
// deleted as write-only, but the SNES struct kept the meaningful state.
static void cmd_get_interrupt_state(const char *args) {
    if (!g_snes) { send_fmt("{\"error\":\"snes not available\"}"); return; }
    Snes *s = g_snes;
    send_fmt("{\"inNmi\":%s,\"inIrq\":%s,\"inVblank\":%s,"
             "\"nmiEnabled\":%s,\"hIrqEnabled\":%s,\"vIrqEnabled\":%s,"
             "\"autoJoyRead\":%s,\"hPos\":%u,\"vPos\":%u,"
             "\"hTimer\":%u,\"vTimer\":%u,\"autoJoyTimer\":%u}",
             s->inNmi       ? "true" : "false",
             s->inIrq       ? "true" : "false",
             s->inVblank    ? "true" : "false",
             s->nmiEnabled  ? "true" : "false",
             s->hIrqEnabled ? "true" : "false",
             s->vIrqEnabled ? "true" : "false",
             s->autoJoyRead ? "true" : "false",
             s->hPos, s->vPos, s->hTimer, s->vTimer, s->autoJoyTimer);
}

extern void snes_catchup_stats(uint64_t *calls, uint64_t *cycles);
extern uint64_t g_apu_timer0_total_ticks;
static void cmd_get_v2_cpu(const char *args) {
    uint64_t cu_calls = 0, cu_cycles = 0;
    snes_catchup_stats(&cu_calls, &cu_cycles);
    send_fmt("{\"A\":\"0x%04x\",\"B\":\"0x%02x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
             "\"S\":\"0x%04x\",\"D\":\"0x%04x\",\"DB\":\"0x%02x\",\"PB\":\"0x%02x\","
             "\"P\":\"0x%02x\",\"m\":%d,\"x\":%d,\"e\":%d,"
             "\"N\":%d,\"V\":%d,\"Z\":%d,\"C\":%d,\"I\":%d,\"D_flag\":%d,"
             "\"main_cycles\":%llu,\"catchup_calls\":%llu,\"catchup_cycles\":%llu,"
             "\"spc_pc\":\"0x%04x\",\"timer0_ticks\":%llu,"
             "\"timer0_target\":%d,\"timer0_div\":%d,\"timer0_cnt\":%d,\"timer0_en\":%d}",
             g_cpu.A, cpu_read_b(&g_cpu), g_cpu.X, g_cpu.Y,
             g_cpu.S, g_cpu.D, g_cpu.DB, g_cpu.PB,
             g_cpu.P, g_cpu.m_flag, g_cpu.x_flag, g_cpu.emulation,
             g_cpu._flag_N, g_cpu._flag_V, g_cpu._flag_Z, g_cpu._flag_C,
             g_cpu._flag_I, g_cpu._flag_D,
             (unsigned long long)g_main_cpu_cycles_estimate,
             (unsigned long long)cu_calls, (unsigned long long)cu_cycles,
             g_snes && g_snes->apu && g_snes->apu->spc ? g_snes->apu->spc->pc : 0,
             (unsigned long long)g_apu_timer0_total_ticks,
             g_snes && g_snes->apu ? g_snes->apu->timer[0].target : 0,
             g_snes && g_snes->apu ? g_snes->apu->timer[0].divider : 0,
             g_snes && g_snes->apu ? g_snes->apu->timer[0].counter : 0,
             g_snes && g_snes->apu ? g_snes->apu->timer[0].enabled : 0);
}

static void cmd_get_cpu_state(const char *args) {
    if (!g_snes_cpu) { send_fmt("{\"error\":\"cpu not available\"}"); return; }
    Cpu *c = g_snes_cpu;
    send_fmt("{\"a\":\"0x%04x\",\"x\":\"0x%04x\",\"y\":\"0x%04x\","
             "\"sp\":\"0x%04x\",\"pc\":\"0x%04x\",\"dp\":\"0x%04x\","
             "\"k\":\"0x%02x\",\"db\":\"0x%02x\","
             "\"c\":%s,\"z\":%s,\"v\":%s,\"n\":%s,"
             "\"i\":%s,\"d\":%s,\"xf\":%s,\"mf\":%s,\"e\":%s,"
             "\"func\":\"%s\"}",
             c->a, c->x, c->y, c->sp, c->pc, c->dp, c->k, c->db,
             c->c ? "true" : "false", c->z ? "true" : "false",
             c->v ? "true" : "false", c->n ? "true" : "false",
             c->i ? "true" : "false", c->d ? "true" : "false",
             c->xf ? "true" : "false", c->mf ? "true" : "false",
             c->e ? "true" : "false",
             g_last_recomp_func ? g_last_recomp_func : "?");
}

static void cmd_get_dma_state(const char *args) {
    if (!g_dma) { send_fmt("{\"error\":\"dma not available\"}"); return; }
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"channels\":[");
    for (int ch = 0; ch < 8; ch++) {
        DmaChannel *dc = &g_dma->channel[ch];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"ch\":%d,\"bAdr\":\"0x%02x\",\"aAdr\":\"0x%04x\",\"aBank\":\"0x%02x\","
            "\"size\":%d,\"mode\":%d,"
            "\"dmaActive\":%s,\"hdmaActive\":%s,\"fixed\":%s,"
            "\"decrement\":%s,\"indirect\":%s,\"fromB\":%s,"
            "\"tableAdr\":\"0x%04x\",\"indBank\":\"0x%02x\","
            "\"repCount\":%d,\"offIndex\":%d,"
            "\"doTransfer\":%s,\"terminated\":%s}",
            ch ? "," : "", ch, dc->bAdr, dc->aAdr, dc->aBank,
            dc->size, dc->mode,
            dc->dmaActive ? "true" : "false", dc->hdmaActive ? "true" : "false",
            dc->fixed ? "true" : "false", dc->decrement ? "true" : "false",
            dc->indirect ? "true" : "false", dc->fromB ? "true" : "false",
            dc->tableAdr, dc->indBank, dc->repCount, dc->offIndex,
            dc->doTransfer ? "true" : "false", dc->terminated ? "true" : "false");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

static void cmd_get_apu_state(const char *args) {
    if (!g_snes || !g_snes->apu || !g_snes->apu->spc) {
        send_fmt("{\"error\":\"apu not available\"}"); return;
    }
    Spc *s = g_snes->apu->spc;
    Apu *a = g_snes->apu;
    send_fmt("{\"spc\":{\"a\":\"0x%02x\",\"x\":\"0x%02x\",\"y\":\"0x%02x\","
             "\"sp\":\"0x%02x\",\"pc\":\"0x%04x\","
             "\"c\":%s,\"z\":%s,\"v\":%s,\"n\":%s,"
             "\"i\":%s,\"h\":%s,\"p\":%s,\"b\":%s},"
             "\"inPorts\":[\"0x%02x\",\"0x%02x\",\"0x%02x\",\"0x%02x\",\"0x%02x\",\"0x%02x\"],"
             "\"outPorts\":[\"0x%02x\",\"0x%02x\",\"0x%02x\",\"0x%02x\"],"
             "\"timer\":[{\"target\":%d,\"counter\":%d,\"enabled\":%s},"
             "{\"target\":%d,\"counter\":%d,\"enabled\":%s},"
             "{\"target\":%d,\"counter\":%d,\"enabled\":%s}]}",
             s->a, s->x, s->y, s->sp, s->pc,
             s->c ? "true" : "false", s->z ? "true" : "false",
             s->v ? "true" : "false", s->n ? "true" : "false",
             s->i ? "true" : "false", s->h ? "true" : "false",
             s->p ? "true" : "false", s->b ? "true" : "false",
             a->inPorts[0], a->inPorts[1], a->inPorts[2], a->inPorts[3], a->inPorts[4], a->inPorts[5],
             a->outPorts[0], a->outPorts[1], a->outPorts[2], a->outPorts[3],
             a->timer[0].target, a->timer[0].counter, a->timer[0].enabled ? "true" : "false",
             a->timer[1].target, a->timer[1].counter, a->timer[1].enabled ? "true" : "false",
             a->timer[2].target, a->timer[2].counter, a->timer[2].enabled ? "true" : "false");
}

static void cmd_dump_apu_ram(const char *args) {
    if (!g_snes || !g_snes->apu) { send_fmt("{\"error\":\"apu not available\"}"); return; }
    unsigned int addr = 0, len = 65536;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 65536) len = 65536;
    if (addr + len > 65536) { send_fmt("{\"error\":\"out of range\"}"); return; }
    if (s_client_sock == SOCKET_INVALID) return;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"", addr, len);
    send(s_client_sock, hdr, (int)strlen(hdr), 0);
    send_hex_blob(g_snes->apu->ram + addr, len);
    send(s_client_sock, "\"}\n", 3, 0);
}

// ---- Extended ring buffer query commands ----

static void cmd_get_frame_extended(const char *args) {
    int frame_num = 0;
    sscanf(args, "%d", &frame_num);
    FrameRecord *r = find_frame(frame_num);
    if (!r) {
        send_fmt("{\"error\":\"frame %d not in buffer\"}", frame_num);
        return;
    }
    if (s_client_sock == SOCKET_INVALID) return;

    // Build JSON with cpu, ppu, dma as structured fields; cgram/oam/zeropage as hex blobs
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf),
        "{\"frame\":%d,"
        "\"cpu\":{\"a\":\"0x%04x\",\"x\":\"0x%04x\",\"y\":\"0x%04x\","
        "\"sp\":\"0x%04x\",\"pc\":\"0x%04x\",\"dp\":\"0x%04x\","
        "\"k\":\"0x%02x\",\"db\":\"0x%02x\",\"flags\":\"0x%02x\",\"e\":%d},"
        "\"ppu\":{\"inidisp\":\"0x%02x\",\"bgmode\":%d,\"mosaic\":\"0x%02x\","
        "\"obsel\":\"0x%02x\",\"setini\":\"0x%02x\","
        "\"screenEnabled\":[\"0x%02x\",\"0x%02x\"],"
        "\"cgadsub\":\"0x%02x\",\"cgwsel\":\"0x%02x\","
        "\"hScroll\":[%d,%d,%d,%d],\"vScroll\":[%d,%d,%d,%d],"
        "\"fixedColor\":\"0x%04x\",\"vramPointer\":\"0x%04x\"},",
        r->frame_number,
        r->cpu.a, r->cpu.x, r->cpu.y, r->cpu.sp, r->cpu.pc, r->cpu.dp,
        r->cpu.k, r->cpu.db, r->cpu.flags, r->cpu.e,
        r->ppu.inidisp, r->ppu.bgmode & 7, r->ppu.mosaic,
        r->ppu.obsel, r->ppu.setini,
        r->ppu.screenEnabled[0], r->ppu.screenEnabled[1],
        r->ppu.cgadsub, r->ppu.cgwsel,
        r->ppu.hScroll[0], r->ppu.hScroll[1], r->ppu.hScroll[2], r->ppu.hScroll[3],
        r->ppu.vScroll[0], r->ppu.vScroll[1], r->ppu.vScroll[2], r->ppu.vScroll[3],
        r->ppu.fixedColor, r->ppu.vramPointer);
    send(s_client_sock, buf, pos, 0);

    // DMA channels (incl. HDMA state fields captured per-frame)
    pos = snprintf(buf, sizeof(buf), "\"dma\":[");
    for (int ch = 0; ch < 8; ch++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"bAdr\":\"0x%02x\",\"aBank\":\"0x%02x\",\"mode\":%d,\"flags\":\"0x%02x\","
            "\"aAdr\":\"0x%04x\",\"size\":%d,"
            "\"tableAdr\":\"0x%04x\",\"indBank\":\"0x%02x\","
            "\"repCount\":%d,\"offIndex\":%d}",
            ch ? "," : "", r->dma[ch].bAdr, r->dma[ch].aBank, r->dma[ch].mode,
            r->dma[ch].flags, r->dma[ch].aAdr, r->dma[ch].size,
            r->dma[ch].tableAdr, r->dma[ch].indBank,
            r->dma[ch].repCount, r->dma[ch].offIndex);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],");
    send(s_client_sock, buf, pos, 0);

    // Interrupt / timing state
    pos = snprintf(buf, sizeof(buf),
        "\"irq\":{\"inNmi\":%s,\"inIrq\":%s,\"inVblank\":%s,"
        "\"nmiEnabled\":%s,\"hIrqEnabled\":%s,\"vIrqEnabled\":%s,"
        "\"autoJoyRead\":%s,\"hPos\":%u,\"vPos\":%u,"
        "\"hTimer\":%u,\"vTimer\":%u,\"autoJoyTimer\":%u},",
        r->irq.inNmi       ? "true" : "false",
        r->irq.inIrq       ? "true" : "false",
        r->irq.inVblank    ? "true" : "false",
        r->irq.nmiEnabled  ? "true" : "false",
        r->irq.hIrqEnabled ? "true" : "false",
        r->irq.vIrqEnabled ? "true" : "false",
        r->irq.autoJoyRead ? "true" : "false",
        r->irq.hPos, r->irq.vPos, r->irq.hTimer, r->irq.vTimer,
        r->irq.autoJoyTimer);
    send(s_client_sock, buf, pos, 0);

    // CGRAM as hex blob
    send(s_client_sock, "\"cgram\":\"", 9, 0);
    send_hex_blob((const uint8_t *)r->cgram, 512);

    // OAM as hex blob
    send(s_client_sock, "\",\"oam\":\"", 9, 0);
    send_hex_blob((const uint8_t *)r->oam, 512);

    // High OAM as hex blob
    send(s_client_sock, "\",\"highOam\":\"", 13, 0);
    send_hex_blob(r->highOam, 32);

    // Zero page as hex blob
    send(s_client_sock, "\",\"zeropage\":\"", 14, 0);
    send_hex_blob(r->zeropage, 256);

    // Game state WRAM $1000-$1FFF as hex blob
    send(s_client_sock, "\",\"wram_1000\":\"", 15, 0);
    send_hex_blob(r->wram_1000, 4096);

    send(s_client_sock, "\"}\n", 3, 0);
}

static void cmd_get_frame_range_extended(const char *args) {
    int start = 0, end = 0;
    sscanf(args, "%d %d", &start, &end);
    if (end - start > 500) end = start + 500;
    char buf[32768];
    int pos = snprintf(buf, sizeof(buf), "{\"frames\":[");
    for (int f = start; f <= end && pos < 30000; f++) {
        FrameRecord *r = find_frame(f);
        if (!r) continue;
        uint8_t dma_active = 0;
        for (int ch = 0; ch < 8; ch++)
            if (r->dma[ch].flags & 0x03) dma_active |= (1 << ch);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,"
            "\"cpu_a\":\"0x%04x\",\"cpu_x\":\"0x%04x\",\"cpu_y\":\"0x%04x\","
            "\"cpu_sp\":\"0x%04x\",\"cpu_db\":\"0x%02x\",\"cpu_flags\":\"0x%02x\","
            "\"ppu_mode\":%d,\"ppu_inidisp\":\"0x%02x\","
            "\"ppu_hscroll\":[%d,%d,%d,%d],\"ppu_vscroll\":[%d,%d,%d,%d],"
            "\"dma_active\":\"0x%02x\",\"mode\":\"0x%02x\"}",
            (f > start && pos > 12) ? "," : "",
            r->frame_number,
            r->cpu.a, r->cpu.x, r->cpu.y, r->cpu.sp, r->cpu.db, r->cpu.flags,
            r->ppu.bgmode & 7, r->ppu.inidisp,
            r->ppu.hScroll[0], r->ppu.hScroll[1], r->ppu.hScroll[2], r->ppu.hScroll[3],
            r->ppu.vScroll[0], r->ppu.vScroll[1], r->ppu.vScroll[2], r->ppu.vScroll[3],
            dma_active, r->snap[21]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* v2 trace ring access via debug-server. */
static void cmd_trace_dump(const char *args) {
    int n = 256;
    sscanf(args ? args : "", "%d", &n);
    cpu_trace_dump_recent("trace cmd", n);
    send_fmt("{\"ok\":true,\"dumped\":%d}", n);
}

/* TCP-readable query of the always-on g_cpu_trace_ring. Walks BACKWARDS
 * from the most-recent event (idx=g_cpu_trace_idx-1) up to `count`
 * events, optionally filtered by event_type and bank/addr range.
 *
 * Args (whitespace-separated, all optional, all hex unless prefixed):
 *     count=N             — max events to emit (default 64, max 4096)
 *     skip=N              — skip first N matching events (default 0)
 *     event=N             — only events with this event_type (CPU_TR_* enum)
 *     bank=XX             — for WRAM_WRITE: extra1>>8 must equal bank
 *     addr_lo=XXXX        — for WRAM_WRITE: addr (low 16 of pc24) >= lo
 *     addr_hi=XXXX        — for WRAM_WRITE: addr <= hi
 *     frame_lo=N          — only events at/after this host frame
 *     frame_hi=N          — only events at/before this host frame
 *     before_idx=N        — start scan from absolute idx N-1 (default = g_cpu_trace_idx)
 *
 * Each emitted event is JSON: idx, event, frame, pc24, A,X,Y,S,D, DB,PB,P,m,x,
 * extra0, extra1.
 */
static void cmd_trace_get_v2(const char *args) {
#if SNESRECOMP_TRACE
    int count = 64;
    int skip = 0;
    int event_filter = -1;
    int bank_filter = -1;
    unsigned int addr_lo = 0, addr_hi = 0xFFFF;
    int frame_lo = INT_MIN;
    int frame_hi = INT_MAX;
    long long before_idx_arg = -1;
    if (args) {
        const char *p;
        if ((p = strstr(args, "count=")) != NULL) sscanf(p + 6, "%d", &count);
        if ((p = strstr(args, "skip=")) != NULL) sscanf(p + 5, "%d", &skip);
        if ((p = strstr(args, "event=")) != NULL) sscanf(p + 6, "%d", &event_filter);
        if ((p = strstr(args, "bank=")) != NULL) { unsigned int b = 0; if (sscanf(p + 5, "%x", &b) == 1) bank_filter = (int)b; }
        if ((p = strstr(args, "addr_lo=")) != NULL) sscanf(p + 8, "%x", &addr_lo);
        if ((p = strstr(args, "addr_hi=")) != NULL) sscanf(p + 8, "%x", &addr_hi);
        if ((p = strstr(args, "frame_lo=")) != NULL) sscanf(p + 9, "%d", &frame_lo);
        if ((p = strstr(args, "frame_hi=")) != NULL) sscanf(p + 9, "%d", &frame_hi);
        if ((p = strstr(args, "before_idx=")) != NULL) sscanf(p + 11, "%lld", &before_idx_arg);
    }
    if (count > 4096) count = 4096;
    if (count < 1) count = 1;

    extern uint64_t g_cpu_trace_idx;
    uint64_t end_idx = (before_idx_arg >= 0)
        ? (uint64_t)before_idx_arg
        : g_cpu_trace_idx;

    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"end_idx\":%llu,\"events\":[",
        (unsigned long long)end_idx);
    int emitted = 0;
    int seen_matching = 0;
    /* Bound scan distance to avoid crawling the whole 1M ring on bad filter. */
    int max_scan = g_cpu_trace_capacity;
    for (int i = 1; i <= max_scan && emitted < count; i++) {
        if ((uint64_t)i > end_idx) break;
        uint64_t abs_idx = end_idx - i;
        int slot = (int)(abs_idx & (g_cpu_trace_capacity - 1));
        CpuTraceEvent *e = &g_cpu_trace_ring[slot];
        /* Skip if filter mismatches. */
        if (event_filter >= 0 && e->event_type != (uint8_t)event_filter) continue;
        if (e->frame < frame_lo || e->frame > frame_hi) continue;
        if (e->event_type == CPU_TR_WRAM_WRITE) {
            /* B2 (2026-05-01): explicit bank + addr16 fields land
             * directly on the event. addr_lo/addr_hi range filter
             * now works for WRAM_WRITE. */
            if (bank_filter >= 0 && e->bank != (uint8_t)bank_filter) continue;
            if (e->addr16 < addr_lo || e->addr16 > addr_hi) continue;
        }
        if (skip > 0 && seen_matching < skip) { seen_matching++; continue; }
        seen_matching++;

        if (pos > (int)sizeof(buf) - 512) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"idx\":%llu,\"event\":%u,\"frame\":%d,\"pc24\":\"0x%06x\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,"
            "\"extra0\":\"0x%02x\",\"extra1\":\"0x%04x\","
            "\"bank\":\"0x%02x\",\"addr16\":\"0x%04x\",\"width\":%u,"
            "\"old_value\":\"0x%04x\",\"new_value\":\"0x%04x\","
            "\"hash\":\"0x%08x\"}",
            emitted ? "," : "",
            (unsigned long long)abs_idx,
            (unsigned)e->event_type, e->frame, e->pc24,
            e->A, e->X, e->Y, e->S, e->D,
            e->DB, e->PB, e->P, e->M, e->XF,
            e->extra0, e->extra1,
            e->bank, e->addr16, e->width,
            e->old_value, e->new_value,
            e->native_func_id_or_hash);
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", emitted);
    send_line(buf);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Arm the scoped one-shot WRAM tripwire. Captures structured snapshot
 * on first matching write that has scope_substr in the recomp stack. */
static void cmd_tripwire_arm(const char *args) {
#if SNESRECOMP_TRACE
    if (!args || !*args) {
        send_fmt("{\"error\":\"usage: tripwire_arm bank addr_lo addr_hi [scope=<substr>]\"}");
        return;
    }
    unsigned int bank = 0, addr_lo = 0, addr_hi = 0;
    int n = sscanf(args, "%x %x %x", &bank, &addr_lo, &addr_hi);
    if (n < 3) {
        send_fmt("{\"error\":\"usage: tripwire_arm bank addr_lo addr_hi [scope=<substr>]\"}");
        return;
    }
    char scope[SCOPED_TRIPWIRE_FUNC_LEN] = {0};
    const char *p = strstr(args, "scope=");
    if (p) {
        p += 6;
        int i = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i < SCOPED_TRIPWIRE_FUNC_LEN - 1) {
            scope[i++] = *p++;
        }
    }
    cpu_trace_arm_scoped_tripwire((uint8_t)bank, (uint16_t)addr_lo,
                                  (uint16_t)addr_hi, scope[0] ? scope : NULL);
    send_fmt("{\"ok\":true,\"bank\":\"0x%02x\",\"addr_lo\":\"0x%04x\","
             "\"addr_hi\":\"0x%04x\",\"scope\":\"%s\"}",
             bank & 0xFF, addr_lo & 0xFFFF, addr_hi & 0xFFFF, scope);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_tripwire_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    ScopedTripwire *t = &g_scoped_tripwire;
    static char buf[16384];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"triggered\":%u,\"scope\":\"%s\","
        "\"bank\":\"0x%02x\",\"addr_lo\":\"0x%04x\",\"addr_hi\":\"0x%04x\"",
        t->armed, t->triggered, t->scope_substr,
        t->bank, t->addr_lo, t->addr_hi);
    if (t->triggered) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"main_cycles\":%llu,\"trace_idx\":%llu,"
            "\"block_counter\":%llu,"
            "\"hit_addr\":\"0x%04x\",\"hit_val\":\"0x%02x\","
            "\"hit_byte_in_word\":%u,\"width\":%u,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,"
            "\"recent_block_pc24\":\"0x%06x\","
            "\"recent_func_pc24\":\"0x%06x\","
            "\"last_func\":\"%s\",\"stack\":[",
            t->frame, (unsigned long long)t->main_cycles,
            (unsigned long long)t->trace_idx,
            (unsigned long long)t->block_counter,
            t->hit_addr, t->hit_val, t->hit_byte_in_word, t->width_seen,
            t->A, t->X, t->Y, t->S, t->D,
            t->DB, t->PB, t->P, t->m_flag, t->x_flag, t->e_flag,
            t->recent_block_pc24, t->recent_func_pc24,
            t->last_func_name);
        for (int i = 0; i < t->stack_depth; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", t->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"dp_0080\":\"");
        for (int i = 0; i < SCOPED_TRIPWIRE_DP_BYTES; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", t->dp_snapshot[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\",\"gm_0100\":\"");
        for (int i = 0; i < SCOPED_TRIPWIRE_GM_BYTES; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", t->gm_snapshot[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\",\"dp_low\":\"");
        for (int i = 0; i < 32; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", t->dp_low_snapshot[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_tripwire_disarm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_disarm_scoped_tripwire();
    send_fmt("{\"ok\":true}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* ── Boundary auditor TCP commands ──────────────────────────────────────
 *
 * boundary_get count=N [skip=N] [kind=ENTRY|EXIT|ANY] [func=substr]
 *   Walk the boundary ring backward from g_boundary_idx and emit JSON.
 *   `kind` filter: 0=ENTRY, 1=EXIT, default ANY.
 *   `func` filter: substring match against event name (case-sensitive).
 *
 * db_trip_arm target=C0   one-shot arm the DB tripwire on target_db.
 * db_trip_get             return the snapshot if triggered.
 * db_trip_disarm          disarm without re-init.
 */
static void cmd_boundary_get(const char *args) {
#if SNESRECOMP_TRACE
    int count = 64;
    int skip = 0;
    int kind_filter = -1;       /* -1 = any */
    char func_filter[BOUNDARY_NAME_LEN] = {0};
    if (args) {
        const char *p;
        if ((p = strstr(args, "count=")) != NULL) sscanf(p + 6, "%d", &count);
        if ((p = strstr(args, "skip=")) != NULL) sscanf(p + 5, "%d", &skip);
        if ((p = strstr(args, "kind=")) != NULL) {
            const char *k = p + 5;
            if (!strncmp(k, "ENTRY", 5)) kind_filter = BD_ENTRY;
            else if (!strncmp(k, "EXIT", 4)) kind_filter = BD_EXIT;
            else if (!strncmp(k, "ANY", 3)) kind_filter = -1;
        }
        if ((p = strstr(args, "func=")) != NULL) {
            p += 5;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i < BOUNDARY_NAME_LEN - 1) {
                func_filter[i++] = *p++;
            }
        }
    }
    if (count > 4096) count = 4096;
    if (count < 1) count = 1;

    extern uint64_t g_boundary_idx;
    extern uint64_t g_boundary_capacity;
    extern BoundaryEvent *g_boundary_ring;
    if (!g_boundary_ring || !g_boundary_capacity) {
        send_fmt("{\"error\":\"boundary ring not allocated\"}");
        return;
    }

    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"end_idx\":%llu,\"events\":[",
        (unsigned long long)g_boundary_idx);
    int emitted = 0;
    int seen_matching = 0;
    int max_scan = (int)g_boundary_capacity;
    for (int i = 1; i <= max_scan && emitted < count; i++) {
        if ((uint64_t)i > g_boundary_idx) break;
        uint64_t abs_idx = g_boundary_idx - (uint64_t)i;
        int slot = (int)(abs_idx & (g_boundary_capacity - 1));
        BoundaryEvent *e = &g_boundary_ring[slot];
        if (kind_filter >= 0 && e->kind != (uint8_t)kind_filter) continue;
        if (func_filter[0] && !strstr(e->name, func_filter)) continue;
        if (skip > 0 && seen_matching < skip) { seen_matching++; continue; }
        seen_matching++;

        if (pos > (int)sizeof(buf) - 512) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"seq\":%llu,\"entry_seq\":%llu,\"frame\":%d,"
            "\"kind\":%u,\"name\":\"%s\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"depth\":%u}",
            emitted ? "," : "",
            (unsigned long long)e->seq,
            (unsigned long long)e->entry_seq,
            e->frame, (unsigned)e->kind, e->name,
            e->A, e->X, e->Y, e->S, e->D,
            e->DB, e->PB, e->P, e->m_flag, e->x_flag,
            (unsigned)e->stack_depth);
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", emitted);
    send_line(buf);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* dispatch_log_get [count=N] [offset=N]
 *   Dump cpu_dispatch_pc call log (PEI-trampoline diagnostic).
 *   count=N (default 64, max 256): events to return, most recent first.
 *   offset=N: skip N most-recent events.
 *   Each event: pc24, mx_idx (0..3=M0X0/M0X1/M1X0/M1X1), found
 *   (1=table hit, 0=miss), mirror (1=found via LoROM bank mirror),
 *   frame.
 */
typedef struct DispatchLogEntry_ DispatchLogEntry_;
struct DispatchLogEntry_ {
    uint32_t pc24;
    uint32_t source_pc24;
    const char *func_name;
    uint8_t  mx_idx, found, mirror, pad;
    uint32_t frame;
};
unsigned cpu_dispatch_log_count(void);
const DispatchLogEntry_ *cpu_dispatch_log_at(unsigned i);

static void cmd_dispatch_log_get(const char *args) {
    int count = 64;
    int offset = 0;
    if (args) {
        const char *p = strstr(args, "count=");
        if (p) sscanf(p + 6, "%d", &count);
        p = strstr(args, "offset=");
        if (p) sscanf(p + 7, "%d", &offset);
    }
    if (count < 1) count = 1;
    if (count > 256) count = 256;
    if (offset < 0) offset = 0;
    unsigned total = cpu_dispatch_log_count();
    static char buf[16384];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"total\":%u,\"events\":[", total);
    int emitted = 0;
    /* Walk backwards from most recent. */
    unsigned start = total;
    for (int k = 0; k < offset && start > 0; k++) start--;
    for (int k = 0; k < count && start > 0; k++) {
        start--;
        const DispatchLogEntry_ *e = cpu_dispatch_log_at(start);
        if (e == NULL) break;
        if (pos > (int)sizeof(buf) - 256) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"frame\":%u,\"pc24\":\"0x%06x\",\"source_pc24\":\"0x%06x\","
            "\"func\":\"%s\",\"mx\":%u,"
            "\"found\":%u,\"mirror\":%u}",
            emitted ? "," : "",
            e->frame, e->pc24, e->source_pc24,
            e->func_name ? e->func_name : "?",
            (unsigned)e->mx_idx,
            (unsigned)e->found, (unsigned)e->mirror);
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", emitted);
    send_line(buf);
}

static void cmd_db_trip_arm(const char *args) {
#if SNESRECOMP_TRACE
    unsigned int target = 0xC0;
    if (args) {
        const char *p = strstr(args, "target=");
        if (p) sscanf(p + 7, "%x", &target);
    }
    cpu_trace_arm_db_tripwire((uint8_t)(target & 0xFF));
    send_fmt("{\"ok\":true,\"target_db\":\"0x%02x\"}", target & 0xFF);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_db_trip_disarm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_disarm_db_tripwire();
    send_fmt("{\"ok\":true}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_db_trip_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    DbTripwire *t = &g_db_tripwire;
    static char buf[262144];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"triggered\":%u,\"target_db\":\"0x%02x\"",
        t->armed, t->triggered, t->target_db);
    if (t->triggered) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"trip_pc24\":\"0x%06x\","
            "\"old_db\":\"0x%02x\",\"new_db\":\"0x%02x\","
            "\"trip_event_type\":%u,"
            "\"trip_boundary_seq\":%llu,\"trip_trace_idx\":%llu,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,"
            "\"last_func\":\"%s\",\"stack\":[",
            t->frame, t->trip_pc24, t->old_db, t->new_db,
            t->trip_event_type,
            (unsigned long long)t->trip_boundary_seq,
            (unsigned long long)t->trip_trace_idx,
            t->A, t->X, t->Y, t->S, t->D,
            t->DB, t->PB, t->P, t->m_flag, t->x_flag, t->e_flag,
            t->last_func);
        for (int i = 0; i < t->stack_depth; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", t->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"boundary_history\":[");
        for (int i = 0; i < t->bd_count; i++) {
            BoundaryEvent *e = &t->bd_history[i];
            if (pos > (int)sizeof(buf) - 512) break;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"seq\":%llu,\"entry_seq\":%llu,\"frame\":%d,"
                "\"kind\":%u,\"name\":\"%s\","
                "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
                "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
                "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\","
                "\"depth\":%u}",
                i ? "," : "",
                (unsigned long long)e->seq,
                (unsigned long long)e->entry_seq,
                e->frame, (unsigned)e->kind, e->name,
                e->A, e->X, e->Y, e->S, e->D,
                e->DB, e->PB,
                (unsigned)e->stack_depth);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"dbpb_history\":[");
        for (int i = 0; i < t->dbpb_count; i++) {
            CpuDbpbEvent *d = &t->dbpb_history[i];
            if (pos > (int)sizeof(buf) - 256) break;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"pc24\":\"0x%06x\",\"event_type\":%u,\"reg_id\":%u,"
                "\"old_val\":\"0x%02x\",\"new_val\":\"0x%02x\",\"S\":\"0x%04x\"}",
                i ? "," : "",
                d->pc24, d->event_type, d->reg_id,
                d->old_val, d->new_val, d->S);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Stack-drift tripwire — fires on the FIRST function exit (post-frame
 * frame_min) where exit_S != entry_S AND exit_kind == NORMAL. Distinct
 * from the DB tripwire (which catches DB→<value> transitions): this
 * catches structural-invariant violations regardless of register state.
 * Auto-arms at boot via cpu_trace_arm_default_watches with frame_min=400. */
static void cmd_stack_drift_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    StackDriftTripwire *t = &g_stack_drift_tripwire;
    static char buf[262144];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"triggered\":%u,\"frame_min\":%d",
        t->armed, t->triggered, t->frame_min);
    if (t->triggered) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"func\":\"%s\","
            "\"entry_S\":\"0x%04x\",\"exit_S\":\"0x%04x\",\"s_delta\":%d,"
            "\"entry_seq\":%llu,\"exit_seq\":%llu,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,\"stack\":[",
            t->frame, t->func_name,
            t->entry_S, t->exit_S, t->s_delta,
            (unsigned long long)t->entry_seq,
            (unsigned long long)t->exit_seq,
            t->A, t->X, t->Y, t->S, t->D,
            t->DB, t->PB, t->P, t->m_flag, t->x_flag, t->e_flag);
        for (int i = 0; i < t->stack_depth; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", t->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"boundary_history\":[");
        for (int i = 0; i < t->bd_count; i++) {
            BoundaryEvent *e = &t->bd_history[i];
            if (pos > (int)sizeof(buf) - 512) break;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"seq\":%llu,\"entry_seq\":%llu,\"frame\":%d,"
                "\"kind\":%u,\"exit_kind\":%u,\"name\":\"%s\","
                "\"S\":\"0x%04x\",\"DB\":\"0x%02x\",\"PB\":\"0x%02x\","
                "\"depth\":%u}",
                i ? "," : "",
                (unsigned long long)e->seq,
                (unsigned long long)e->entry_seq,
                e->frame, (unsigned)e->kind, (unsigned)e->exit_kind,
                e->name,
                e->S, e->DB, e->PB,
                (unsigned)e->stack_depth);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Function-name entry watch: arm by name, then query for the first-hit
 * snapshot (frame, pc, registers, full recomp call stack). The watch is
 * one-shot — re-arming via func_watch_arm clears the prior snapshot.
 *
 *   func_watch_arm <function_name>     — arm with name (FNV-1a hashed)
 *   func_watch_get                     — return captured snapshot or empty
 */
static void cmd_func_watch_arm(const char *args) {
#if SNESRECOMP_TRACE
    if (!args || !*args) {
        send_fmt("{\"error\":\"func_watch_arm needs <name>\"}");
        return;
    }
    /* Copy into static storage so the cstr lives past this fn's stack. */
    static char watched_name[80];
    int i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\n' && i < (int)sizeof(watched_name) - 1) {
        watched_name[i] = args[i]; i++;
    }
    watched_name[i] = 0;
    cpu_trace_set_func_watch(watched_name);
    send_fmt("{\"ok\":1,\"watching\":\"%s\"}", watched_name);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_func_watch_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    FuncWatchHit *h = &g_func_watch_hit;
    static char buf[16384];
    int pos = snprintf(buf, sizeof(buf),
        "{\"captured\":%u", h->captured);
    if (h->captured) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"pc24\":\"0x%06x\",\"name\":\"%s\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,\"stack\":[",
            h->frame, h->pc24, h->name,
            h->A, h->X, h->Y, h->S, h->D,
            h->DB, h->PB, h->P, h->m_flag, h->x_flag, h->e_flag);
        for (int i = 0; i < h->stack_depth; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", h->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Per-instruction stack-op trace: enable / disable / status. Off by default
 * because it produces ~5-10x more events than block trace at typical play
 * rates. Turn on for short windows (e.g. arm just before frame 800, query
 * the resulting CPU_TR_STACK_OP events around frame 822 trip). */
static void cmd_stack_op_enable(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    g_stack_op_trace_enabled = 1;
    send_fmt("{\"ok\":1,\"enabled\":1}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}
static void cmd_stack_op_disable(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    g_stack_op_trace_enabled = 0;
    send_fmt("{\"ok\":1,\"enabled\":0}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}
static void cmd_stack_op_status(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    send_fmt("{\"ok\":1,\"enabled\":%u}", g_stack_op_trace_enabled);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* phantom_trap_get — JSON dump of the SMC-phantom-PC trap state.
 * Returns the armed count and any captured hits (each with snapshot,
 * recomp stack at first hit, and 64-deep block-PC history). Used to
 * empirically verify that the 11 cf_debt_report-flagged CALL_INDIRECT
 * sites NEVER fire as real instruction starts. */
static void cmd_phantom_trap_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    static char buf[65536];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%d,\"hit_count\":%d,\"hits\":[",
        g_phantom_trap_armed_count, g_phantom_trap_hit_count);
    for (int i = 0; i < g_phantom_trap_hit_count; i++) {
        PhantomTrapHit *h = &g_phantom_trap_hits[i];
        if (!h->captured) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"pc24\":\"0x%06x\",\"label\":\"%s\","
            "\"first_frame\":%d,\"first_block_idx\":%llu,"
            "\"repeat_count\":%d,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,"
            "\"stack\":[",
            i ? "," : "", h->pc24, h->label,
            h->first_frame, (unsigned long long)h->first_block_idx,
            h->repeat_count,
            h->A, h->X, h->Y, h->S, h->D, h->DB, h->PB, h->P,
            h->m_flag, h->x_flag, h->e_flag);
        for (int j = 0; j < h->stack_depth; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", j ? "," : "", h->stack[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"block_history\":[");
        for (int j = 0; j < h->block_history_depth; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"0x%06x\"", j ? "," : "", h->block_history[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* phantom_trap_clear — disarm everything and zero the captured hits.
 * After this, phantom_trap_get returns armed=0, hit_count=0. Use to
 * re-arm a different set via TCP. */
static void cmd_phantom_trap_clear(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_phantom_disarm_all();
    send_fmt("{\"ok\":1,\"armed\":0,\"hit_count\":0}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* unresolved_goto_get — JSON dump of the unresolved-cross-fn-goto
 * trap ring. Keyed on (func_name + source_pc24) so each gen variant
 * that hits the goto gets its own slot. */
static void cmd_unresolved_goto_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    static char buf[65536];
    int pos = snprintf(buf, sizeof(buf),
        "{\"hit_count\":%d,\"hits\":[",
        g_unresolved_goto_hit_count);
    for (int i = 0; i < g_unresolved_goto_hit_count; i++) {
        UnresolvedGotoHit *h = &g_unresolved_goto_hits[i];
        if (!h->captured) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"source_pc24\":\"0x%06x\",\"target_pc24\":\"0x%06x\","
            "\"func\":\"%s\",\"target_label\":\"%s\","
            "\"first_frame\":%d,\"first_block_idx\":%llu,"
            "\"repeat_count\":%d,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,\"stack\":[",
            i ? "," : "", h->source_pc24, h->target_pc24,
            h->func_name, h->target_label,
            h->first_frame, (unsigned long long)h->first_block_idx,
            h->repeat_count,
            h->A, h->X, h->Y, h->S, h->D, h->DB, h->PB, h->P,
            h->m_flag, h->x_flag, h->e_flag);
        for (int j = 0; j < h->stack_depth; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", j ? "," : "", h->stack[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"block_history\":[");
        for (int j = 0; j < h->block_history_depth; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"0x%06x\"", j ? "," : "", h->block_history[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* unresolved_stub_get — JSON dump of the unresolved-stub trap ring.
 * Each slot is a unique stub function name that actually got called
 * at runtime (every fire is a sign of a phantom variant or a real
 * cross-bank call into a non-cfg ROM bank). */
static void cmd_unresolved_stub_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    static char buf[65536];
    int pos = snprintf(buf, sizeof(buf),
        "{\"hit_count\":%d,\"hits\":[",
        g_unresolved_stub_hit_count);
    for (int i = 0; i < g_unresolved_stub_hit_count; i++) {
        UnresolvedStubHit *h = &g_unresolved_stub_hits[i];
        if (!h->captured) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"target_pc24\":\"0x%06x\",\"func\":\"%s\","
            "\"first_frame\":%d,\"first_block_idx\":%llu,"
            "\"repeat_count\":%d,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,\"e\":%u,\"stack\":[",
            i ? "," : "", h->target_pc24, h->func_name,
            h->first_frame, (unsigned long long)h->first_block_idx,
            h->repeat_count,
            h->A, h->X, h->Y, h->S, h->D, h->DB, h->PB, h->P,
            h->m_flag, h->x_flag, h->e_flag);
        for (int j = 0; j < h->stack_depth; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", j ? "," : "", h->stack[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"block_history\":[");
        for (int j = 0; j < h->block_history_depth; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"0x%06x\"", j ? "," : "", h->block_history[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* gm14_player_trace_get [count] [from_idx]
 *
 * Dumps rows from the GM14 per-tick player-state ring as one JSON
 * row per line, framed with a header and a trailing "{\"end\":1}" marker.
 *
 * Args:
 *   count     — number of rows to return (default 256, max 8192)
 *   from_idx  — absolute g_gm14_trace_idx to start from (default:
 *               last `count` rows, i.e. (idx - count) clamped to 0).
 *
 * Streamed line-at-a-time so a long history walk doesn't blow a
 * single response buffer. The differ in Python reads lines until
 * end:1.
 */
static void cmd_gm14_player_trace_get(const char *args) {
#if SNESRECOMP_TRACE
    long count = 256;
    long from = -1;
    if (args && *args) {
        char *endp = NULL;
        count = strtol(args, &endp, 0);
        if (endp && *endp) {
            while (*endp == ' ') endp++;
            if (*endp) from = strtol(endp, NULL, 0);
        }
    }
    if (count < 1) count = 1;
    if (count > 8192) count = 8192;

    if (!g_gm14_trace_ring || !g_gm14_trace_capacity) {
        send_fmt("{\"error\":\"gm14 ring not allocated\"}");
        return;
    }
    uint64_t total = g_gm14_trace_idx;
    uint64_t cap   = g_gm14_trace_capacity;
    uint64_t start;
    if (from < 0) {
        start = (total > (uint64_t)count) ? (total - (uint64_t)count) : 0;
    } else {
        start = (uint64_t)from;
        if (start > total) start = total;
    }
    /* Clamp start to the oldest still-resident row. */
    if (total > cap && start < (total - cap)) start = total - cap;
    uint64_t end = start + (uint64_t)count;
    if (end > total) end = total;

    send_fmt("{\"header\":1,\"total_idx\":%llu,\"capacity\":%llu,\"tick_ordinal\":%llu,"
             "\"start\":%llu,\"end\":%llu}",
             (unsigned long long)total, (unsigned long long)cap,
             (unsigned long long)g_gm14_tick_ordinal,
             (unsigned long long)start, (unsigned long long)end);
    char line[512];
    for (uint64_t i = start; i < end; i++) {
        Gm14TickRow *r = &g_gm14_trace_ring[i & (cap - 1)];
        snprintf(line, sizeof(line),
            "{\"i\":%llu,\"t\":%llu,\"f\":%d,\"k\":%u,\"gm\":%u,"
            "\"in\":[%u,%u,%u,%u],"
            "\"st71\":%u,\"st77\":%u,\"xs\":%d,\"ys\":%d,"
            "\"px\":%u,\"py\":%u,"
            "\"yoshi\":[%u,%u,%u],"
            "\"scr\":[%u,%u],\"cam\":[%u,%u],"
            "\"spr9\":{\"st\":%u,\"n\":%u,\"x\":%u,\"y\":%u}}",
            (unsigned long long)i,
            (unsigned long long)r->tick_ordinal,
            (int)r->host_frame, r->kind, r->gamemode,
            r->in_15, r->in_16, r->in_17, r->in_18,
            r->st_71, r->st_77, (int)r->xspeed, (int)r->yspeed,
            r->pos_x, r->pos_y,
            r->yoshi_187A, r->yoshi_18E2, r->yoshi_1888,
            r->scroll_1A, r->scroll_1C, r->cam_1462, r->cam_1464,
            r->spr9_status, r->spr9_number, r->spr9_x, r->spr9_y);
        send_line(line);
    }
    send_fmt("{\"end\":1}");
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* gm14_player_trace_clear — reset the ring (idx=0, ordinal=0). Useful
 * before re-running attract from a known boot state. */
static void cmd_gm14_player_trace_clear(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_gm14_clear();
    send_fmt("{\"ok\":1}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* phantom_trap_arm_smc — re-arm the canonical SMC-phantom set. Useful
 * after a phantom_trap_clear during a single run. */
static void cmd_phantom_trap_arm_smc(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_phantom_arm_smc_set();
    send_fmt("{\"ok\":1,\"armed\":%d}", g_phantom_trap_armed_count);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Stack-drift control: arm / clear / disarm. The auditor auto-arms at
 * boot with frame_min=400 to skip boot prolog noise. After a trip, the
 * boundary ring is FROZEN to preserve the seq window for inspection.
 * To investigate later events (e.g. a death scene many frames after a
 * benign earlier imbalance), call stack_drift_clear [frame_min] — that
 * resets the trip flag AND unfreezes the boundary ring, so subsequent
 * imbalances and the full ongoing call boundary stream are captured
 * again.
 *
 * Args:
 *   stack_drift_arm <frame_min>           — full arm: clear+set frame_min+arm
 *   stack_drift_clear [frame_min]         — clear trip; keep armed; optional
 *                                           new frame_min (default: keep)
 *   stack_drift_disarm                    — disarm; ring stays unfrozen
 */
static void cmd_stack_drift_arm(const char *args) {
#if SNESRECOMP_TRACE
    extern uint8_t g_boundary_frozen;
    int frame_min = 0;
    if (args && *args) frame_min = (int)strtol(args, NULL, 0);
    cpu_trace_arm_stack_drift_tripwire((int32_t)frame_min);
    g_boundary_frozen = 0;
    send_fmt("{\"ok\":1,\"armed\":1,\"frame_min\":%d}", frame_min);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_stack_drift_clear(const char *args) {
#if SNESRECOMP_TRACE
    extern uint8_t g_boundary_frozen;
    StackDriftTripwire *t = &g_stack_drift_tripwire;
    int new_min = t->frame_min;
    if (args && *args) new_min = (int)strtol(args, NULL, 0);
    /* Clear trip + boundary-history snapshot, keep armed flag. */
    uint8_t was_armed = t->armed ? 1 : 0;
    memset(t, 0, sizeof(*t));
    t->armed = was_armed;
    t->frame_min = (int32_t)new_min;
    /* Unfreeze the live boundary ring so post-clear events resume. The
     * DB tripwire shares the same g_boundary_frozen flag — only unfreeze
     * if THAT tripwire isn't holding a snapshot we'd lose. Conservative:
     * unfreeze unconditionally — DB trip can be re-armed/cleared similarly
     * via db_trip_arm. */
    g_boundary_frozen = 0;
    send_fmt("{\"ok\":1,\"armed\":%u,\"frame_min\":%d,\"triggered\":0}",
             t->armed, new_min);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_stack_drift_disarm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    extern uint8_t g_boundary_frozen;
    cpu_trace_disarm_stack_drift_tripwire();
    g_boundary_frozen = 0;
    send_fmt("{\"ok\":1,\"armed\":0}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* ── Inspection-freeze (freeze-at-frame) ──────────────────────────────
 * Deliberately stop ALL rings (cpu_trace + boundary + WRAM + rtstrace +
 * oamblk) so a window survives a runaway/stuck game. Unlike tripwires,
 * this also freezes the always-on cpu_trace ring (g_freeze_capture).
 *   freeze_at_frame <N>  — freeze the first cpu_trace_block at frame >= N
 *   freeze_now           — freeze immediately
 *   unfreeze             — resume all rings
 *   freeze_status        — report flags + current indices
 * The env var SNESRECOMP_FREEZE_AT_FRAME=<N> arms this from boot (no race). */
static void cmd_freeze_at_frame(const char *args) {
    extern uint8_t g_freeze_capture; extern int g_freeze_at_frame;
    extern uint8_t g_boundary_frozen;
    int n = (args && *args) ? (int)strtol(args, NULL, 0) : -1;
    g_freeze_at_frame = n;
    g_freeze_capture = 0;   /* re-arm */
    g_boundary_frozen = 0;
    send_fmt("{\"ok\":1,\"freeze_at_frame\":%d}", n);
}

static void cmd_freeze_now(const char *args) {
    (void)args;
    extern uint8_t g_freeze_capture; extern uint8_t g_boundary_frozen;
    g_freeze_capture = 1; g_boundary_frozen = 1;
    send_fmt("{\"ok\":1,\"frozen\":1}");
}

static void cmd_unfreeze(const char *args) {
    (void)args;
    extern uint8_t g_freeze_capture; extern int g_freeze_at_frame;
    extern uint8_t g_boundary_frozen;
    g_freeze_capture = 0; g_boundary_frozen = 0; g_freeze_at_frame = -1;
    send_fmt("{\"ok\":1,\"frozen\":0}");
}

static void cmd_freeze_status(const char *args) {
    (void)args;
    extern uint8_t g_freeze_capture; extern int g_freeze_at_frame;
    extern uint8_t g_boundary_frozen;
    extern uint64_t g_cpu_trace_idx; extern uint64_t g_boundary_idx;
    extern int snes_frame_counter;
    send_fmt("{\"freeze_capture\":%u,\"boundary_frozen\":%u,"
             "\"freeze_at_frame\":%d,\"frame\":%d,"
             "\"trace_idx\":%llu,\"boundary_idx\":%llu}",
             g_freeze_capture, g_boundary_frozen, g_freeze_at_frame,
             snes_frame_counter,
             (unsigned long long)g_cpu_trace_idx,
             (unsigned long long)g_boundary_idx);
}

/* ── Static M/X claim verifier TCP commands ────────────────────────────
 *
 * Companion to docs/ABSTRACT_INTERPRETATION_GAPS.md. Accumulating
 * always-on observability surface for decoder-soundness mismatches.
 * One record per (func_name, runtime_m, runtime_x). NOT a tripwire.
 *
 *   mx_claim_check_arm                    — clear + arm the table
 *   mx_claim_check_disarm                 — disarm
 *   mx_claim_check_get                    — summary (counts only)
 *   mx_claim_get_all                      — dump every record
 */
static void cmd_mx_claim_check_arm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_arm_mx_claim_check();
    send_fmt("{\"ok\":1,\"armed\":1}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_mx_claim_check_disarm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_disarm_mx_claim_check();
    send_fmt("{\"ok\":1,\"armed\":0}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_mx_claim_check_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    MxClaimTable *t = &g_mx_claim_table;
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"record_count\":%u,\"record_cap\":%u,"
        "\"total_hits\":%llu,\"overflow_count\":%llu}",
        (unsigned)t->armed,
        (unsigned)t->record_count,
        (unsigned)MX_CLAIM_RECORD_CAP,
        (unsigned long long)t->total_hits,
        (unsigned long long)t->overflow_count);
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_mx_claim_get_all(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    MxClaimTable *t = &g_mx_claim_table;
    /* Each record can produce ~1200 chars (stack of 16 names × ~64 chars
     * + scalar fields). 256 × 1300 = ~330 KiB upper bound. */
    static char buf[400 * 1024];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"armed\":%u,\"record_count\":%u,\"record_cap\":%u,"
        "\"total_hits\":%llu,\"overflow_count\":%llu,\"records\":[",
        (unsigned)t->armed,
        (unsigned)t->record_count,
        (unsigned)MX_CLAIM_RECORD_CAP,
        (unsigned long long)t->total_hits,
        (unsigned long long)t->overflow_count);
    for (uint32_t i = 0; i < t->record_count && pos < (int)sizeof(buf) - 2048; i++) {
        MxClaimRecord *r = &t->records[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"func\":\"%s\","
            "\"claimed_m\":%u,\"claimed_x\":%u,"
            "\"runtime_m\":%u,\"runtime_x\":%u,"
            "\"hit_count\":%llu,"
            "\"first_frame\":%d,\"first_pc24\":\"0x%06x\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"e\":%u,\"stack\":[",
            i ? "," : "",
            r->func_name,
            (unsigned)r->claimed_m, (unsigned)r->claimed_x,
            (unsigned)r->runtime_m, (unsigned)r->runtime_x,
            (unsigned long long)r->hit_count,
            r->first_frame, r->first_pc24,
            r->A, r->X, r->Y, r->S, r->D,
            r->DB, r->PB, r->P, (unsigned)r->e_flag);
        for (int s = 0; s < r->stack_depth && pos < (int)sizeof(buf) - 1024; s++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", s ? "," : "", r->stack[s]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Async m_flag/x_flag write tripwire — catches mutations of
 * cpu->m_flag or cpu->x_flag that occur between two cpu_trace_block
 * hooks WITHOUT a corresponding cpu_trace_px_record (i.e. without
 * going through an emitted SEP/REP/PHP/PLP/RTI/XCE). The DA49 entry
 * in ISSUES.md is the canonical example of the class this catches.
 *
 *   mx_async_check_arm                    — arm tripwire (clear trip)
 *   mx_async_check_get                    — return current trip snapshot
 *   mx_async_check_disarm                 — disarm tripwire
 */
static void cmd_mx_async_check_arm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_arm_mx_async_check();
    send_fmt("{\"ok\":1,\"armed\":1}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_mx_async_check_disarm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_disarm_mx_async_check();
    send_fmt("{\"ok\":1,\"armed\":0}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* offrails_get — JSON dump of every off-rails bucket. Each bucket is
 * a unique (tag, high-16-of-hint) combination; entries accumulate
 * hit_count + last_frame/last_hint without further stderr output.
 *
 * Example response:
 *   {"count":2,"buckets":[
 *     {"tag":"RomPtr-invalid","first_frame":4581,"last_frame":4612,
 *      "first_hint":"0x00BB93AB","last_hint":"0x00BBA17F",
 *      "hit_count":42,"stack_top":"Layer1SpecialScrolling08_..."}
 *     ,{...}]}
 */
static void cmd_offrails_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    int n = cpu_trace_offrails_count();
    static char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"count\":%d,\"buckets\":[", n);
    for (int i = 0; i < n && pos < (int)sizeof(buf) - 256; i++) {
        const OffRailsBucket *b = cpu_trace_offrails_bucket(i);
        if (!b) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"tag\":\"%s\",\"first_frame\":%d,\"last_frame\":%d,"
            "\"first_hint\":\"0x%08X\",\"last_hint\":\"0x%08X\","
            "\"hit_count\":%llu,\"stack_top\":\"%s\"}",
            i ? "," : "",
            b->tag, b->first_frame, b->last_frame,
            b->first_hint, b->last_hint,
            (unsigned long long)b->hit_count,
            b->stack_top);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_mx_async_check_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    MxAsyncTrip *t = &g_mx_async_trip;
    static char buf[8192];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"triggered\":%u,\"px_mutation_count\":%llu",
        (unsigned)t->armed, (unsigned)t->triggered,
        (unsigned long long)g_px_mutation_count);
    if (t->triggered) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"block_pc24\":\"0x%06x\","
            "\"prev_m\":%u,\"prev_x\":%u,\"new_m\":%u,\"new_x\":%u,"
            "\"px_count_at_trip\":%llu,"
            "\"func\":\"%s\",\"stack\":[",
            t->frame, t->block_pc24,
            (unsigned)t->prev_m, (unsigned)t->prev_x,
            (unsigned)t->new_m, (unsigned)t->new_x,
            (unsigned long long)t->px_count_at_trip,
            t->func_name);
        for (int i = 0; i < t->stack_depth && pos < (int)sizeof(buf) - 256; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", t->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* NLR diagnostic — non-rotating counters that survive cpu_trace ring
 * rotation. Answers "did any NLR-pattern block ever execute?" and
 * "did any Return ever consume a non-zero pending_skip?" — questions
 * the rotating ring can't answer once it's wrapped. */
static void cmd_nlr_diag(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    NlrDiag *d = &g_nlr_diag;
    static char buf[16384];
    int pos = snprintf(buf, sizeof(buf),
        "{\"site_exec_count\":%llu,"
        "\"pending_skip_writes\":%llu,"
        "\"pending_skip_reads_zero\":%llu,"
        "\"pending_skip_reads_nonzero\":%llu,"
        "\"first_writer\":{\"captured\":%u",
        (unsigned long long)d->site_exec_count,
        (unsigned long long)d->pending_skip_writes,
        (unsigned long long)d->pending_skip_reads_zero,
        (unsigned long long)d->pending_skip_reads_nonzero,
        d->first_writer_captured);
    if (d->first_writer_captured) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"pc24\":\"0x%06x\",\"value\":%u,\"func\":\"%s\"",
            d->first_writer_frame, d->first_writer_pc24,
            d->first_writer_value, d->first_writer_func);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "},\"first_consumer\":{\"captured\":%u",
        d->first_consumer_captured);
    if (d->first_consumer_captured) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"pc24\":\"0x%06x\",\"value\":%u,\"func\":\"%s\"",
            d->first_consumer_frame, d->first_consumer_pc24,
            d->first_consumer_value, d->first_consumer_func);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "},\"per_site\":[");
    for (int i = 0; i < d->per_site_used; i++) {
        if (pos > (int)sizeof(buf) - 256) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"label\":\"%s\",\"count\":%llu}",
            i ? "," : "", d->per_site_label[i],
            (unsigned long long)d->per_site_count[i]);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* DMA tripwire — fires on the FIRST $420B write where any active
 * channel has VRAM destination in $7000-$8FFF AND source bank $05.
 * Captures rich snapshot for offline analysis. */
static void cmd_dma_trip_get(const char *args) {
    (void)args;
    DmaTripwire *t = &s_dma_tripwire;
    static char buf[16384];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"triggered\":%u",
        t->armed, t->triggered);
    if (t->triggered) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"frame\":%d,\"main_cycles\":%llu,\"trace_idx\":%llu,"
            "\"channel\":%u,\"dmap\":\"0x%02x\",\"bbus\":\"0x%02x\","
            "\"a_addr\":\"0x%04x\",\"a_bank\":\"0x%02x\","
            "\"size\":\"0x%04x\","
            "\"vram_addr\":\"0x%04x\",\"vmain\":\"0x%02x\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,"
            "\"last_func\":\"%s\",\"stack\":[",
            t->frame, (unsigned long long)t->main_cycles,
            (unsigned long long)t->trace_idx,
            t->channel, t->dmap, t->bbus, t->a_addr, t->a_bank,
            t->size, t->vram_addr, t->vmain,
            t->A, t->X, t->Y, t->S, t->D,
            t->DB, t->PB, t->P, t->m_flag, t->x_flag,
            t->last_func);
        for (int i = 0; i < t->stack_depth; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", t->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"dma_regs\":\"");
        for (int i = 0; i < 0x80; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x",
                            t->dma_regs_snap[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\",\"dp_low\":\"");
        for (int i = 0; i < 32; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x",
                            t->dp_low_snap[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    send_line(buf);
}

/* P.X tripwire commands. The tripwire is auto-armed at process startup
 * (in cpu_trace_arm_default_watches), so by the time TCP connects, the
 * snapshot may already be frozen. pxwatch_get fetches the captured
 * snapshot whether or not it's already triggered. */
static void cmd_pxwatch_arm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_arm_px_tripwire();
    send_fmt("{\"ok\":true}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_pxwatch_disarm(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_disarm_px_tripwire();
    send_fmt("{\"ok\":true}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_pxwatch_clear(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    cpu_trace_clear_px_tripwire();
    send_fmt("{\"ok\":true}");
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

static void cmd_pxwatch_get(const char *args) {
    (void)args;
#if SNESRECOMP_TRACE
    PxTripwire *t = &g_px_tripwire;
    static const char *kind_name[] = {
        "REP", "SEP", "PLP", "RTI", "PHP", "p_to_mirrors", "mirrors_to_p", "XCE"
    };
    static char buf[65536];
    int pos = snprintf(buf, sizeof(buf),
        "{\"armed\":%u,\"triggered\":%u,\"pmut_count\":%u,\"breadcrumb_count\":%u",
        t->armed, t->triggered,
        (unsigned)t->pmut_count, (unsigned)t->breadcrumb_count);

    if (t->triggered) {
        const char *src = (t->trip_event.source_kind < 8)
            ? kind_name[t->trip_event.source_kind] : "?";
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"trip\":{\"pc24\":\"0x%06x\",\"source\":\"%s\","
            "\"old_p\":\"0x%02x\",\"new_p\":\"0x%02x\","
            "\"old_x_flag\":%u,\"new_x_flag\":%u,\"S\":\"0x%04x\","
            "\"trace_idx\":%u,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"D\":\"0x%04x\",\"DB\":\"0x%02x\",\"PB\":\"0x%02x\","
            "\"P\":\"0x%02x\",\"m\":%u,\"x\":%u,\"e\":%u,"
            "\"last_func\":\"%s\",\"stack\":[",
            t->trip_event.pc24, src,
            t->trip_event.old_p, t->trip_event.new_p,
            t->trip_event.old_x_flag, t->trip_event.new_x_flag,
            t->trip_event.S, t->trip_trace_idx,
            t->A, t->X, t->Y, t->D, t->DB, t->PB,
            t->P, t->m_flag, t->x_flag, t->e_flag,
            t->last_func);
        for (int i = 0; i < t->stack_depth; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", i ? "," : "", t->stack[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }

    /* Emit P-mutation ring (newest-first within the snapshot's own ring) */
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"pmut\":[");
    uint32_t emit_count = t->pmut_count;
    if (emit_count > 0) {
        uint32_t newest = (t->pmut_write_idx - 1) % PX_TRIPWIRE_PMUT_RING;
        for (uint32_t i = 0; i < emit_count; i++) {
            uint32_t slot = (newest + PX_TRIPWIRE_PMUT_RING - i) % PX_TRIPWIRE_PMUT_RING;
            PxPMutEvent *e = &t->pmut_ring[slot];
            const char *src = (e->source_kind < 8) ? kind_name[e->source_kind] : "?";
            if (pos > (int)sizeof(buf) - 256) break;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"pc24\":\"0x%06x\",\"source\":\"%s\","
                "\"old_p\":\"0x%02x\",\"new_p\":\"0x%02x\","
                "\"old_x\":%u,\"new_x\":%u,\"S\":\"0x%04x\"}",
                i ? "," : "",
                e->pc24, src, e->old_p, e->new_p,
                e->old_x_flag, e->new_x_flag, e->S);
        }
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"breadcrumbs\":[");
    for (uint32_t i = 0; i < t->breadcrumb_count; i++) {
        PxBreadcrumb *bc = &t->breadcrumbs[i];
        if (pos > (int)sizeof(buf) - 256) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"label\":\"%s\",\"marker\":\"0x%08x\","
            "\"P\":\"0x%02x\",\"m\":%u,\"x\":%u,\"e\":%u,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\"}",
            i ? "," : "",
            bc->label, bc->marker,
            bc->P, bc->m_flag, bc->x_flag, bc->e_flag,
            bc->A, bc->X, bc->Y, bc->S, bc->D, bc->DB, bc->PB);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}
static void cmd_trace_dbpb(const char *args) {
    (void)args;
    cpu_trace_dump_dbpb("dbpb cmd");
    send_fmt("{\"ok\":true}");
}
static void cmd_trace_clear(const char *args) {
    (void)args;
    cpu_trace_clear();
    send_fmt("{\"ok\":true}");
}
static void cmd_set_db_watch(const char *args) {
    /* args: "<hex byte> [enable=1]" — sets the tripwire on that DB value */
    unsigned int byte = 0;
    int enable = 1;
    if (sscanf(args ? args : "", "%x %d", &byte, &enable) < 1) {
        send_fmt("{\"error\":\"usage: set_db_watch <hex byte> [0|1]\"}");
        return;
    }
    cpu_trace_set_db_watch((uint8_t)byte, enable);
    send_fmt("{\"ok\":true,\"db\":\"0x%02X\",\"enabled\":%d}", byte & 0xFF, enable);
}

static void cmd_arm_watches(const char *args) {
    (void)args;
    cpu_trace_arm_default_watches();
    send_fmt("{\"ok\":true,\"armed\":\"defaults\"}");
}

/* Arm a WRAM-address watch.
 * args: "<bank-hex> <addr-hex> <width> [match_value=0|1] [value-hex] [enable=1]"
 *   bank      e.g. 7E
 *   addr      e.g. 008c
 *   width     1 or 2
 *   match_value  0 = fire on any write, 1 = only when new low byte == value
 *   value     hex byte to match against (only used when match_value==1)
 *   enable    0 = disarm matching slot, 1 = arm
 * Example for "fire when $7E:008c becomes $57":
 *   set_wram_watch 7E 008c 1 1 57 1
 * Example for "fire on any write to $7E:008c":
 *   set_wram_watch 7E 008c 1
 */
static void cmd_set_wram_watch(const char *args) {
    unsigned int bank = 0, addr = 0, width = 1, match_value = 0, value = 0;
    int enable = 1;
    int n = sscanf(args ? args : "", "%x %x %u %u %x %d",
                   &bank, &addr, &width, &match_value, &value, &enable);
    if (n < 3) {
        send_fmt("{\"error\":\"usage: set_wram_watch <bank> <addr> "
                 "<width 1|2> [match_value 0|1] [value-hex] [enable 0|1]\"}");
        return;
    }
    cpu_trace_set_wram_watch((uint8_t)bank, (uint16_t)addr, (int)width,
                             (int)match_value, (uint8_t)value, enable);
    send_fmt("{\"ok\":true,\"bank\":\"0x%02X\",\"addr\":\"0x%04X\","
             "\"width\":%u,\"match_value\":%u,\"value\":\"0x%02X\","
             "\"enabled\":%d}",
             bank & 0xFF, addr & 0xFFFF, width, match_value,
             value & 0xFF, enable);
}

static void cmd_clear_wram_watches(const char *args) {
    (void)args;
    cpu_trace_clear_wram_watches();
    send_fmt("{\"ok\":true,\"cleared\":true}");
}

/* block_watch_arm <pc24_hex> <ram_off1>[,<ram_off2>,...] [max_hits=8]
 *
 * Arms a block-keyed sampler at `pc24`. On every entry to that block,
 * captures registers + the byte at each listed g_ram offset + recomp
 * call stack (up to 8 frames). Stops capturing after `max_hits`. Use
 * `block_watch_get [slot]` to read back captured hits.
 *
 * Example — capture state at $01:9081 (the SpriteDisableObjInt gate
 * inside SubUpdateSprPos), reading $1FE2+9 ($1FEB), $1540+9 ($1549),
 * $1558+9 ($1561), $15B8+9 ($15C1):
 *   block_watch_arm 019081 1FEB,1549,1561,15C1 12
 */
static void cmd_block_watch_arm(const char *args) {
#if SNESRECOMP_TRACE
    if (!args || !*args) {
        send_fmt("{\"error\":\"usage: block_watch_arm <pc24_hex> "
                 "<ram_off1>[,<ram_off2>,...] [max_hits=8]\"}");
        return;
    }
    unsigned int pc = 0;
    char addrs_str[256] = {0};
    int max_hits = 8;
    int n = sscanf(args, "%x %255s %d", &pc, addrs_str, &max_hits);
    if (n < 2) {
        send_fmt("{\"error\":\"need pc24 and at least one addr\"}");
        return;
    }
    int32_t addrs[BLOCK_WATCH_ADDRS_MAX];
    for (int i = 0; i < BLOCK_WATCH_ADDRS_MAX; i++) addrs[i] = -1;
    int n_addrs = 0;
    char *tok = strtok(addrs_str, ",");
    while (tok && n_addrs < BLOCK_WATCH_ADDRS_MAX) {
        addrs[n_addrs++] = (int32_t)strtoul(tok, NULL, 16);
        tok = strtok(NULL, ",");
    }
    cpu_trace_block_watch_arm((uint32_t)pc, addrs, n_addrs, max_hits);
    send_fmt("{\"ok\":1,\"pc24\":\"0x%06x\",\"n_addrs\":%d,\"max_hits\":%d}",
             pc, n_addrs, max_hits);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* block_watch_get [slot]
 * Dumps captured hits as JSON. Without slot arg, dumps all enabled slots.
 */
static void cmd_block_watch_get(const char *args) {
#if SNESRECOMP_TRACE
    int slot_filter = -1;
    if (args && *args) sscanf(args, "%d", &slot_filter);
    static char buf[1 << 20];   /* 1MB */
    int pos = snprintf(buf, sizeof(buf), "{\"slots\":[");
    int emitted = 0;
    for (int i = 0; i < BLOCK_WATCH_MAX; i++) {
        if (slot_filter >= 0 && i != slot_filter) continue;
        BlockWatch *w = &g_block_watches[i];
        if (!w->enabled) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"slot\":%d,\"pc24\":\"0x%06x\",\"hit_count\":%d,"
            "\"max_hits\":%d,\"n_addrs\":%d,\"addrs\":[",
            emitted ? "," : "", i, w->pc24, w->hit_count,
            w->max_hits, w->n_addrs);
        for (int j = 0; j < w->n_addrs; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"0x%05x\"", j ? "," : "", (uint32_t)w->ram_offsets[j]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"events\":[");
        for (int h = 0; h < w->hit_count && pos < (int)sizeof(buf) - 2048; h++) {
            BlockWatchHit *e = &w->hits[h];
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"hit\":%d,\"frame\":%d,"
                "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
                "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
                "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
                "\"m\":%u,\"x\":%u,\"e\":%u,\"vals\":[",
                h ? "," : "", h, e->frame,
                e->A, e->X, e->Y, e->S, e->D,
                e->DB, e->PB, e->P,
                e->m_flag, e->x_flag, e->e_flag);
            for (int v = 0; v < w->n_addrs; v++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s\"0x%02x\"", v ? "," : "", e->vals[v]);
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"stack\":[");
            for (int s = 0; s < e->stack_depth; s++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s\"%s\"", s ? "," : "", e->stack[s]);
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* block_watch_clear [slot]
 * Without slot arg, clears all slots. With slot, clears just that one. */
static void cmd_block_watch_clear(const char *args) {
#if SNESRECOMP_TRACE
    int slot = -1;
    if (args && *args) sscanf(args, "%d", &slot);
    if (slot < 0) cpu_trace_block_watch_clear_all();
    else cpu_trace_block_watch_clear_one(slot);
    send_fmt("{\"ok\":1,\"cleared\":\"%s\"}", slot < 0 ? "all" : "one");
#else
    (void)args;
    send_fmt("{\"error\":\"SNESRECOMP_TRACE not enabled\"}");
#endif
}

/* Dump only WRAM_WRITE events from the trace ring. Each event includes
 * the most-recent function + block context preceding it, so you can
 * attribute the write without dumping the whole ring. Output goes to
 * stderr (the SMW process). args: "<scan_n>" — how far back to scan;
 * 0 = entire ring. */
static void cmd_trace_dump_wram(const char *args) {
    int n = 0;
    sscanf(args ? args : "", "%d", &n);
    cpu_trace_dump_wram("wram cmd", n);
    send_fmt("{\"ok\":true,\"scanned\":%d}", n);
}

/* SPC PC histogram so we can see *exactly* which engine PCs the SPC
 * spends time in. apu.c samples spc->pc once per apu_cycle when SPC
 * starts a new opcode (cpuCyclesLeft was 0). */
extern uint64_t g_spc_pc_histogram[0x10000];
extern int g_spc_pc_max_seen;

static void cmd_get_apu_misc(const char *args) {
    if (!g_snes || !g_snes->apu) { send_fmt("{\"error\":\"apu n/a\"}"); return; }
    send_fmt("{\"romReadable\":%s,\"cycles\":%llu,\"cpuCyclesLeft\":%d}",
             g_snes->apu->romReadable ? "true" : "false",
             (unsigned long long)g_snes->apu->cycles,
             g_snes->apu->cpuCyclesLeft);
}

static void cmd_get_spc_pc_hist(const char *args) {
    /* Find top-32 hottest PCs and report them with their counts. */
    int top_n = 64;
    int top_pcs[64] = {0};
    uint64_t top_counts[64] = {0};
    for (int pc = 0; pc < 0x10000; pc++) {
        uint64_t c = g_spc_pc_histogram[pc];
        if (c == 0) continue;
        /* Insert into top list. */
        int slot = -1;
        for (int s = 0; s < top_n; s++) {
            if (c > top_counts[s]) { slot = s; break; }
        }
        if (slot >= 0) {
            for (int s = top_n - 1; s > slot; s--) {
                top_pcs[s] = top_pcs[s-1];
                top_counts[s] = top_counts[s-1];
            }
            top_pcs[slot] = pc;
            top_counts[slot] = c;
        }
    }
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"max_pc\":\"0x%04x\",\"top\":[", g_spc_pc_max_seen);
    int first = 1;
    for (int i = 0; i < top_n; i++) {
        if (top_counts[i] == 0) break;
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "[\"0x%04x\",%llu]", top_pcs[i],
                        (unsigned long long)top_counts[i]);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* Dump write counts for SPC apu_cpuWrite addresses. Shows whether
 * engine ever touches $F4-$F7 (outPorts), $F1 (control/timer enable), etc. */
extern uint64_t g_spc_write_counts[0x100];
extern uint64_t g_spc_outport_value_counts[4 * 256];
typedef struct { uint8_t adr; uint8_t val; } SpcWriteRec;
extern SpcWriteRec g_spc_recent_outport_writes[32];
extern int g_spc_recent_outport_idx;

static void cmd_get_spc_writes(const char *args) {
    /* Dump (a) per-address counts $F0-$FF, (b) top 8 most-written values
     * for each outPort, (c) recent 32 outPort writes. */
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"writes\":{");
    for (int a = 0xF0; a <= 0xFF; a++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s\"0x%02X\":%llu",
                        (a == 0xF0) ? "" : ",",
                        a, (unsigned long long)g_spc_write_counts[a]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "},\"top_vals\":[");
    for (int port = 0; port < 4; port++) {
        if (port) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        /* Find values with non-zero counts. */
        int count = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"port\":%d,\"vals\":{", port);
        for (int v = 0; v < 256; v++) {
            uint64_t c = g_spc_outport_value_counts[port * 256 + v];
            if (c > 0) {
                if (count) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "\"0x%02X\":%llu", v, (unsigned long long)c);
                count++;
                if (count > 8) break;
            }
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"recent\":[");
    for (int i = 0; i < 32; i++) {
        int idx = (g_spc_recent_outport_idx - 32 + i) & 31;
        if (i) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "[\"0x%02X\",\"0x%02X\"]",
                        g_spc_recent_outport_writes[idx].adr,
                        g_spc_recent_outport_writes[idx].val);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* Post-mortem JSON dump — calls into recomp_post_mortem_dump (in
 * src/post_mortem.c). Declared weakly so projects sharing this
 * debug_server.c don't fail to link; if no implementation is present,
 * the command just emits a sentinel reply. */
extern void recomp_post_mortem_dump(const char *reason, void *fault_info);
static void cmd_post_mortem_dump(const char *args) {
    (void)args;
    recomp_post_mortem_dump("on_demand", NULL);
    send_line("{\"ok\":true,\"path\":\"build/last_run_report.json\"}");
}

// ---- task #7 focused OAM-overflow trace commands ----
static void cmd_oamblk_range(const char *args) {
    unsigned int lo = 0, hi = 0;
    if (sscanf(args, "%x %x", &lo, &hi) < 2 || hi < lo) {
        send_fmt("{\"ok\":false,\"error\":\"usage: oamblk_range <lo> <hi>\"}"); return;
    }
    g_oamblk_lo = lo; g_oamblk_hi = hi;
    s_oamblk.write_idx = 0; s_oamblk.count = 0;
    send_fmt("{\"ok\":true,\"lo\":\"0x%06x\",\"hi\":\"0x%06x\"}", lo, hi);
}
static void cmd_rtstrace_range(const char *args) {
    unsigned int lo = 0, hi = 0;
    if (sscanf(args, "%x %x", &lo, &hi) < 2 || hi < lo) {
        send_fmt("{\"ok\":false,\"error\":\"usage: rtstrace_range <lo> <hi>\"}"); return;
    }
    g_rtst_lo = lo; g_rtst_hi = hi;
    s_rtst.write_idx = 0; s_rtst.count = 0;
    send_fmt("{\"ok\":true,\"lo\":\"0x%06x\",\"hi\":\"0x%06x\"}", lo, hi);
}
static void cmd_get_oamblk(const char *args) {
    int from_frame = 0, to_frame = INT_MAX, limit = 512;
    sscanf(args, "%d %d %d", &from_frame, &to_frame, &limit);
    if (limit > 4000) limit = 4000;
    int start = s_oamblk.count < OAMBLK_TRACE_SIZE ? 0 :
                (int)(s_oamblk.write_idx - OAMBLK_TRACE_SIZE);
    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%u,\"log\":[", s_oamblk.count);
    int n = 0;
    for (unsigned k = 0; k < s_oamblk.count && n < limit; k++) {
        int idx = (start + (int)k) % OAMBLK_TRACE_SIZE;
        int f = s_oamblk.log[idx].frame;
        if (f < from_frame || f > to_frame) continue;
        if (pos > (int)sizeof(buf) - 400) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"pc\":\"0x%06x\",\"fn\":\"%s\",\"par\":\"%s\",\"dep\":%d,"
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\",\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"P\":\"0x%02x\",\"mx\":%d,"
            "\"E3\":\"0x%02x\",\"E4\":\"0x%02x\",\"E6\":\"0x%02x\",\"E8\":\"0x%02x\",\"EA\":\"0x%02x\",\"EB\":\"0x%02x\"}",
            n ? "," : "", f, s_oamblk.log[idx].pc, s_oamblk.log[idx].func, s_oamblk.log[idx].parent,
            s_oamblk.log[idx].depth, s_oamblk.log[idx].a, s_oamblk.log[idx].x, s_oamblk.log[idx].y,
            s_oamblk.log[idx].s, s_oamblk.log[idx].d, s_oamblk.log[idx].db, s_oamblk.log[idx].p,
            s_oamblk.log[idx].mx, s_oamblk.log[idx].e3, s_oamblk.log[idx].e4, s_oamblk.log[idx].e6,
            s_oamblk.log[idx].e8, s_oamblk.log[idx].ea, s_oamblk.log[idx].eb);
        n++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", n);
    send_line(buf);
}
static void cmd_get_rtstrace(const char *args) {
    int from_frame = 0, to_frame = INT_MAX, limit = 512;
    sscanf(args, "%d %d %d", &from_frame, &to_frame, &limit);
    if (limit > 4000) limit = 4000;
    static const char *dec[] = { "HOST_RETURN", "DISPATCH", "MISS_UNWIND", "ANCESTOR_SKIP" };
    int start = s_rtst.count < RTS_TRACE_SIZE ? 0 :
                (int)(s_rtst.write_idx - RTS_TRACE_SIZE);
    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%u,\"log\":[", s_rtst.count);
    int n = 0;
    for (unsigned k = 0; k < s_rtst.count && n < limit; k++) {
        int idx = (start + (int)k) % RTS_TRACE_SIZE;
        int f = s_rtst.log[idx].frame;
        if (f < from_frame || f > to_frame) continue;
        if (pos > (int)sizeof(buf) - 400) break;
        int d = s_rtst.log[idx].decision; if (d < 0 || d > 3) d = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"src\":\"0x%06x\",\"fn\":\"%s\",\"entry_s\":\"0x%04x\","
            "\"ret_s\":\"0x%04x\",\"s_after\":\"0x%04x\",\"popped\":\"0x%06x\","
            "\"hrv\":%d,\"s_eq\":%d,\"found\":%d,\"decision\":\"%s\",\"E3\":\"0x%02x\",\"E4\":\"0x%02x\"}",
            n ? "," : "", f, s_rtst.log[idx].src_pc, s_rtst.log[idx].func, s_rtst.log[idx].entry_s,
            s_rtst.log[idx].ret_s, s_rtst.log[idx].s_after, s_rtst.log[idx].popped_pc,
            s_rtst.log[idx].hrv, s_rtst.log[idx].s_eq, s_rtst.log[idx].found, dec[d],
            s_rtst.log[idx].e3, s_rtst.log[idx].e4);
        n++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", n);
    send_line(buf);
}

/* ---- Audio observability (always-on rings in audio_trace.c) ---- */
#include "audio_trace.h"

/* audio_stats — counters + the most recent per-second snapshots.
 * Args: [snap_count=30] */
static void cmd_audio_stats(const char *args) {
    int want = 30;
    sscanf(args, "%d", &want);
    if (want < 0) want = 0;
    if (want > 256) want = 256;
    AudioTraceStats st;
    audio_trace_get_stats(&st);
    static char buf[65536];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"produced\":%llu,\"produced_cpu\":%llu,\"produced_audio\":%llu,"
        "\"dropped\":%llu,\"drop_runs\":%llu,\"consumed\":%llu,\"consume_calls\":%llu,"
        "\"reg_writes\":%llu,\"kon_writes\":%llu,\"occupancy_highwater\":%u,"
        "\"pace_baseline_cycles\":%llu,\"pace_accumulate_calls\":%llu,"
        "\"pace_consumer_active\":%u,"
        "\"event_count\":%llu,\"snap_count\":%llu,\"snaps\":[",
        (unsigned long long)st.produced, (unsigned long long)st.produced_cpu,
        (unsigned long long)st.produced_audio, (unsigned long long)st.dropped,
        (unsigned long long)st.drop_runs, (unsigned long long)st.consumed,
        (unsigned long long)st.consume_calls, (unsigned long long)st.reg_writes,
        (unsigned long long)st.kon_writes, st.occupancy_highwater,
        (unsigned long long)st.pace_baseline_cycles,
        (unsigned long long)st.pace_accumulate_calls, st.pace_consumer_active,
        (unsigned long long)st.event_count, (unsigned long long)st.snap_count);
    uint64_t first = st.snap_count > (uint64_t)want ? st.snap_count - want : 0;
    static AudioTraceSnap snaps[256];
    uint64_t oldest = 0;
    uint32_t n = audio_trace_copy_snaps(first, (uint32_t)want, snaps, &oldest);
    for (uint32_t i = 0; i < n && pos < (int)sizeof(buf) - 256; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"ms\":%llu,\"prod\":%llu,\"drop\":%llu,\"cons\":%llu,\"occ\":%u}",
            i ? "," : "", (unsigned long long)snaps[i].wall_ms,
            (unsigned long long)snaps[i].produced,
            (unsigned long long)snaps[i].dropped,
            (unsigned long long)snaps[i].consumed, snaps[i].occupancy);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

/* audio_events — DSP reg writes / drop runs / consume events.
 * Args: <first_idx> [max=2000] [reg_only=0] */
static void cmd_audio_events(const char *args) {
    unsigned long long first = 0; int max = 2000, reg_only = 0;
    sscanf(args, "%llu %d %d", &first, &max, &reg_only);
    if (max < 1) max = 1;
    if (max > 8000) max = 8000;
    static AudioTraceEvent ev[8000];
    uint64_t oldest = 0;
    uint32_t n = audio_trace_copy_events(first, (uint32_t)max, ev, &oldest);
    static char buf[1048576];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"oldest\":%llu,\"first\":%llu,\"events\":[",
        (unsigned long long)oldest,
        (unsigned long long)(first < oldest ? oldest : first));
    static const char *tn[] = { "?", "reg", "drop", "consume" };
    int emitted = 0;
    for (uint32_t i = 0; i < n && pos < (int)sizeof(buf) - 256; i++) {
        int t = ev[i].type;
        if (t < 1 || t > 3) t = 0;
        if (reg_only && t != AUDIO_TRACE_EV_REG) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"s\":%llu,\"t\":\"%s\",\"adr\":\"0x%02x\",\"val\":\"0x%02x\","
            "\"aux\":%u,\"p\":%u}",
            emitted ? "," : "", (unsigned long long)ev[i].sample_idx, tn[t],
            ev[i].addr, ev[i].val, ev[i].aux, ev[i].producer);
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"returned\":%d,\"scanned\":%u}",
             emitted, n);
    send_line(buf);
}

/* audio_wav — dump a PCM-ring slice as a WAV file (server-side write).
 * Args: <path> [start_idx=-1 (oldest)] [count=0 (all)] */
static void cmd_audio_wav(const char *args) {
    char path[512] = {0};
    long long start = -1;
    unsigned long long count = 0;
    if (sscanf(args, "%511s %lld %llu", path, &start, &count) < 1 || !path[0]) {
        send_fmt("{\"error\":\"usage: audio_wav <path> [start_idx] [count]\"}");
        return;
    }
    uint64_t out_start = 0, out_count = 0;
    if (audio_trace_dump_wav(path, (int64_t)start, (uint64_t)count,
                             &out_start, &out_count) != 0) {
        send_fmt("{\"error\":\"cannot write %s\"}", path);
        return;
    }
    send_fmt("{\"ok\":true,\"path\":\"%s\",\"start\":%llu,\"count\":%llu}",
             path, (unsigned long long)out_start, (unsigned long long)out_count);
}

typedef struct { const char *name; void (*handler)(const char *args); } CmdEntry;
static const CmdEntry s_commands[] = {
    {"audio_stats",   cmd_audio_stats},
    {"audio_events",  cmd_audio_events},
    {"audio_wav",     cmd_audio_wav},
    {"ping",          cmd_ping},
    {"oam_state",      cmd_oam_state},
    {"oam_write_get",  cmd_oam_write_get},
    {"oam_render_get", cmd_oam_render_get},
    {"oamblk_range",  cmd_oamblk_range},
    {"get_oamblk",    cmd_get_oamblk},
    {"rtstrace_range", cmd_rtstrace_range},
    {"get_rtstrace",  cmd_get_rtstrace},
    {"post_mortem_dump", cmd_post_mortem_dump},
    {"get_v2_cpu",    cmd_get_v2_cpu},
    {"trace_dump",     cmd_trace_dump},
    {"trace_dbpb",     cmd_trace_dbpb},
    {"trace_clear",    cmd_trace_clear},
    {"trace_get_v2",   cmd_trace_get_v2},
    {"tripwire_arm",   cmd_tripwire_arm},
    {"tripwire_get",   cmd_tripwire_get},
    {"tripwire_disarm", cmd_tripwire_disarm},
    {"boundary_get",   cmd_boundary_get},
    {"db_trip_arm",    cmd_db_trip_arm},
    {"db_trip_get",    cmd_db_trip_get},
    {"dispatch_log_get", cmd_dispatch_log_get},
    {"nlr_diag",       cmd_nlr_diag},
    {"stack_drift_get", cmd_stack_drift_get},
    {"stack_drift_arm", cmd_stack_drift_arm},
    {"mx_claim_check_arm", cmd_mx_claim_check_arm},
    {"mx_claim_check_disarm", cmd_mx_claim_check_disarm},
    {"mx_claim_check_get", cmd_mx_claim_check_get},
    {"mx_claim_get_all", cmd_mx_claim_get_all},
    {"mx_async_check_arm", cmd_mx_async_check_arm},
    {"mx_async_check_disarm", cmd_mx_async_check_disarm},
    {"mx_async_check_get", cmd_mx_async_check_get},
    {"offrails_get", cmd_offrails_get},
    {"stack_drift_clear", cmd_stack_drift_clear},
    {"stack_drift_disarm", cmd_stack_drift_disarm},
    {"freeze_at_frame", cmd_freeze_at_frame},
    {"freeze_now",      cmd_freeze_now},
    {"unfreeze",        cmd_unfreeze},
    {"freeze_status",   cmd_freeze_status},
    {"func_watch_arm", cmd_func_watch_arm},
    {"func_watch_get", cmd_func_watch_get},
    {"stack_op_enable",  cmd_stack_op_enable},
    {"stack_op_disable", cmd_stack_op_disable},
    {"stack_op_status",  cmd_stack_op_status},
    {"phantom_trap_get",     cmd_phantom_trap_get},
    {"phantom_trap_clear",   cmd_phantom_trap_clear},
    {"phantom_trap_arm_smc", cmd_phantom_trap_arm_smc},
    {"unresolved_goto_get",  cmd_unresolved_goto_get},
    {"unresolved_stub_get",  cmd_unresolved_stub_get},
    {"gm14_player_trace_get",   cmd_gm14_player_trace_get},
    {"gm14_player_trace_clear", cmd_gm14_player_trace_clear},
    {"db_trip_disarm", cmd_db_trip_disarm},
    {"dma_trip_get",   cmd_dma_trip_get},
    {"pxwatch_arm",    cmd_pxwatch_arm},
    {"pxwatch_disarm", cmd_pxwatch_disarm},
    {"pxwatch_clear",  cmd_pxwatch_clear},
    {"pxwatch_get",    cmd_pxwatch_get},
    {"set_db_watch",   cmd_set_db_watch},
    {"arm_watches",    cmd_arm_watches},
    {"set_wram_watch", cmd_set_wram_watch},
    {"clear_wram_watches", cmd_clear_wram_watches},
    {"block_watch_arm",   cmd_block_watch_arm},
    {"block_watch_get",   cmd_block_watch_get},
    {"block_watch_clear", cmd_block_watch_clear},
    {"trace_dump_wram", cmd_trace_dump_wram},
    {"get_spc_writes", cmd_get_spc_writes},
    {"get_spc_pc_hist", cmd_get_spc_pc_hist},
    {"get_apu_misc",   cmd_get_apu_misc},
    {"frame",         cmd_frame},
    {"read_ram",      cmd_read_ram},
    {"dump_ram",      cmd_dump_ram},
    {"read_sram",     cmd_read_sram},
    {"call_stack",    cmd_call_stack},
    {"watch",         cmd_watch},
    {"unwatch",       cmd_unwatch},
    {"pause",         cmd_pause},
    {"continue",      cmd_continue},
    {"step",          cmd_step},
    {"set_controller",   cmd_set_controller},
    {"get_controller",   cmd_get_controller},
    {"clear_controller", cmd_clear_controller},
    {"func_snap_set",   cmd_func_snap_set},
    {"func_snap_count", cmd_func_snap_count},
    {"func_snap_get_n", cmd_func_snap_get_n},
    {"run_to_frame",  cmd_run_to_frame},
    {"loadstate",     cmd_loadstate},
    {"save_state",    cmd_save_state},
    {"load_state",    cmd_load_state},
    {"invoke_recomp", cmd_invoke_recomp},
    {"write_ram",     cmd_write_ram},
    {"zero_ram",      cmd_zero_ram},
    {"set_cpu",       cmd_set_cpu},
    {"trace_addr",    cmd_trace_addr},
    {"get_trace",     cmd_get_trace},
    {"trace_reg",     cmd_trace_reg},
    {"trace_reg_reset", cmd_trace_reg_reset},
    {"get_reg_trace", cmd_get_reg_trace},
    {"trace_vram",    cmd_trace_vram},
    {"trace_vram_reset", cmd_trace_vram_reset},
    {"get_vram_trace", cmd_get_vram_trace},
    {"get_oracle_vram_trace", cmd_get_oracle_vram_trace},
    {"vram_write_diff", cmd_vram_write_diff},
    {"last_vram_write_to", cmd_last_vram_write_to},
#if SNESRECOMP_REVERSE_DEBUG
    {"trace_wram",        cmd_trace_wram},
    {"trace_wram_reset",  cmd_trace_wram_reset},
    {"get_wram_trace",    cmd_get_wram_trace},
    {"trace_calls",       cmd_trace_calls},
    {"trace_calls_reset", cmd_trace_calls_reset},
    {"get_call_trace",    cmd_get_call_trace},
    {"trace_blocks",       cmd_trace_blocks},
    {"trace_blocks_reset", cmd_trace_blocks_reset},
    {"get_block_trace",    cmd_get_block_trace},
    {"break_add",          cmd_break_add},
    {"break_clear",        cmd_break_clear},
    {"break_list",         cmd_break_list},
    {"break_continue",     cmd_break_continue},
    {"step_block",         cmd_step_block},
    {"parked",             cmd_parked},
    {"watch_add",          cmd_watch_add},
    {"watch_clear",        cmd_watch_clear},
    {"watch_list",         cmd_watch_list},
    {"watch_continue",     cmd_watch_continue},
    {"block_idx_now",      cmd_block_idx_now},
    {"tier3_anchor_on",    cmd_tier3_anchor_on},
    {"tier3_anchor_off",   cmd_tier3_anchor_off},
    {"tier3_anchor_status",cmd_tier3_anchor_status},
    {"wram_at_block",      cmd_wram_at_block},
    {"wram_first_change",  cmd_wram_first_change},
    {"wram_writes_at",     cmd_wram_writes_at},
#endif
    {"trace_range",   cmd_trace_range},
    {"get_trace_range", cmd_get_trace_range},
    {"get_frame",     cmd_get_frame},
    {"frame_range",   cmd_frame_range},
    {"history",       cmd_history_status},
    {"wram_timeseries", cmd_wram_timeseries},
    {"sprite_timeseries", cmd_sprite_timeseries},
    {"profile",       cmd_profile_query},
    {"profile_on",    cmd_profile_on},
    {"profile_off",   cmd_profile_off},
    {"latches",       cmd_latches},
    {"get_functions", cmd_get_functions},
    // Exhaustive state dumps
    {"dump_vram",     cmd_dump_vram},
    {"dump_frame_vram", cmd_dump_frame_vram},
    {"dump_frame_wram", cmd_dump_frame_wram},
    {"dump_cgram",    cmd_dump_cgram},
    {"dump_oam",      cmd_dump_oam},
    {"get_ppu_state", cmd_get_ppu_state},
    {"get_cpu_state", cmd_get_cpu_state},
    {"get_dma_state", cmd_get_dma_state},
    {"get_interrupt_state", cmd_get_interrupt_state},
    {"get_apu_state", cmd_get_apu_state},
    {"dump_apu_ram",  cmd_dump_apu_ram},
    {"screenshot",     cmd_screenshot},
    {"get_frame_extended", cmd_get_frame_extended},
    {"get_frame_range_extended", cmd_get_frame_range_extended},
    {NULL, NULL}
};

static void process_command(char *line) {
    // Trim trailing whitespace
    char *end = line + strlen(line) - 1;
    while (end > line && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;

    // Split command and args
    char *space = strchr(line, ' ');
    const char *args = "";
    if (space) { *space = 0; args = space + 1; }

    for (const CmdEntry *c = s_commands; c->name; c++) {
        if (strcmp(line, c->name) == 0) {
            c->handler(args);
            return;
        }
    }
    send_fmt("{\"error\":\"unknown command\",\"cmd\":\"%s\"}", line);
}

// ---- Public API ----

int debug_server_init(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    InitializeCriticalSection(&s_mutex);
#endif

    s_shutdown = 0;

    s_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_sock == SOCKET_INVALID) return -1;

    // Allow reuse
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CLOSESOCKET(s_listen_sock);
        s_listen_sock = SOCKET_INVALID;
        return -1;
    }

    listen(s_listen_sock, 1);
    set_nonblocking(s_listen_sock);

    memset(s_watchpoints, 0, sizeof(s_watchpoints));

    /* Always-on OAM observability rings — unconditional (small: write ring
     * ~30 MB, render ring ~170 KB), unlike the REVERSE_DEBUG-gated VRAM
     * rings. They record from this point so probes query a recent window
     * rather than arm-then-capture. */
    s_oam_wr.log = (OamWriteEntry *)calloc(OAM_WRITE_RING_ENTRIES, sizeof(OamWriteEntry));
    s_oam_rd.log = (OamRenderEntry *)calloc(OAM_RENDER_RING_ENTRIES, sizeof(OamRenderEntry));
    fprintf(stderr,
        "[debug_server] OAM rings: write %u entries (~%llu MB), render %u snaps (~%llu KB)%s\n",
        OAM_WRITE_RING_ENTRIES,
        (unsigned long long)(((uint64_t)OAM_WRITE_RING_ENTRIES * sizeof(OamWriteEntry)) >> 20),
        OAM_RENDER_RING_ENTRIES,
        (unsigned long long)(((uint64_t)OAM_RENDER_RING_ENTRIES * sizeof(OamRenderEntry)) >> 10),
        (s_oam_wr.log && s_oam_rd.log) ? "" : " [ALLOC FAILED]");

#if SNESRECOMP_REVERSE_DEBUG
    // Always-on WRAM trace: arm a single full-WRAM range so every store
    // is recorded continuously from process start. Probes query the ring
    // backward in history; they never need to "arm trace; run workload;
    // dump trace" — that pattern loses events to attach latency.
    //
    // Cost: WRAM_TRACE_LOG_SIZE = 1M entries × ~80 bytes = ~80 MB
    // resident, plus the per-store hot-path filter. Both are gated on
    // SNESRECOMP_REVERSE_DEBUG (Oracle build only); Release|x64 omits
    // the trace path entirely so this is byte-clean for the shipping
    // exe.
    s_wram_trace.nranges = 1;
    s_wram_trace.ranges[0].lo = 0x00000;
    s_wram_trace.ranges[0].hi = 0x1FFFF;
    s_wram_trace.active = 1;

    // Always-on Tier-2 block-entry trace. Same rationale as the WRAM
    // ring above: probes query the buffer backward; never arm-then-
    // record. cmd_trace_blocks / cmd_trace_blocks_reset still let
    // explicit callers toggle, but the default starts armed.
    s_block_trace.active = 1;

    // Always-on VRAM byte-write trace (recomp + oracle paired). Heap-
    // allocated to escape the 2 GB PE image cap — the existing
    // s_frame_history alone consumes ~1.2 GB of BSS, leaving little
    // room for additional always-on rings. Default capacity ≈ 16M
    // entries × ~160 bytes ≈ 2.6 GB recomp + 128 MB oracle, covering
    // ~30 000 frames (~8 minutes at 60 Hz). Override via
    // SNESRECOMP_VRAM_RING_ENTRIES (decimal). Gated on
    // SNESRECOMP_REVERSE_DEBUG so Release|x64 stays clean.
    {
        uint64_t cap = vram_trace_alloc_rings();
        if (s_vram_trace.log && s_oracle_vram_trace.log) {
            s_vram_trace.nranges = 1;
            s_vram_trace.ranges[0].lo = 0x0000;
            s_vram_trace.ranges[0].hi = 0xFFFF;
            s_vram_trace.active = 1;
            s_oracle_vram_trace.active = 1;
            fprintf(stderr,
                "[debug_server] VRAM rings allocated: %llu entries each "
                "(recomp ~%llu MB, oracle ~%llu MB)\n",
                (unsigned long long)cap,
                (unsigned long long)((cap * sizeof(VramTraceEntry)) >> 20),
                (unsigned long long)((cap * sizeof(OracleVramTraceEntry)) >> 20));
        } else {
            fprintf(stderr,
                "[debug_server] WARNING: failed to allocate VRAM rings "
                "(%llu entries); reduce SNESRECOMP_VRAM_RING_ENTRIES.\n",
                (unsigned long long)cap);
        }
    }
#endif

    // Spawn background network thread
#ifdef _WIN32
    s_thread = (HANDLE)_beginthreadex(NULL, 0, debug_server_thread, NULL, 0, NULL);
    if (!s_thread) {
        fprintf(stderr, "[debug_server] Failed to create network thread\n");
        CLOSESOCKET(s_listen_sock);
        s_listen_sock = SOCKET_INVALID;
        return -1;
    }
#else
    if (pthread_create(&s_thread, NULL, debug_server_thread, NULL) != 0) {
        fprintf(stderr, "[debug_server] Failed to create network thread\n");
        CLOSESOCKET(s_listen_sock);
        s_listen_sock = SOCKET_INVALID;
        return -1;
    }
    s_thread_created = 1;
#endif

    fprintf(stderr, "[debug_server] Listening on port %d (threaded)\n", port);
    return 0;
}

void debug_server_set_ram(uint8_t *ram, uint32_t ram_size) {
    s_ram = ram;
    s_ram_size = ram_size;
}

static void check_watchpoints(void) {
    if (!s_ram) return;
    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) continue;
        uint8_t cur = s_ram[s_watchpoints[i].addr];
        if (cur != s_watchpoints[i].prev_val) {
            // Always log to stderr (captures even without TCP client)
            fprintf(stderr, "[WATCH] @%d 0x%x: %02x->%02x func=%s\n",
                    snes_frame_counter, s_watchpoints[i].addr,
                    s_watchpoints[i].prev_val, cur,
                    g_last_recomp_func ? g_last_recomp_func : "?");
            // Also send to TCP client if connected
            if (s_client_sock != SOCKET_INVALID)
                send_fmt("{\"watchpoint\":{\"addr\":\"0x%x\",\"old\":\"0x%02x\",\"new\":\"0x%02x\","
                         "\"frame\":%d,\"func\":\"%s\"}}",
                         s_watchpoints[i].addr, s_watchpoints[i].prev_val, cur,
                         snes_frame_counter,
                         g_last_recomp_func ? g_last_recomp_func : "?");
            s_watchpoints[i].prev_val = cur;
        }
    }
}

static void try_recv_and_process(void) {
    if (s_client_sock == SOCKET_INVALID) return;

    // Non-blocking recv
    int n = recv(s_client_sock, s_recv_buf + s_recv_len,
                 (int)(sizeof(s_recv_buf) - s_recv_len - 1), 0);
    if (n > 0) {
        s_recv_len += n;
        s_recv_buf[s_recv_len] = 0;

        // Process complete lines
        char *nl;
        while ((nl = strchr(s_recv_buf, '\n')) != NULL) {
            *nl = 0;
            process_command(s_recv_buf);
            int remaining = s_recv_len - (int)(nl + 1 - s_recv_buf);
            memmove(s_recv_buf, nl + 1, remaining);
            s_recv_len = remaining;
            s_recv_buf[s_recv_len] = 0;
        }
    } else if (n == 0) {
        // Client disconnected
        fprintf(stderr, "[debug_server] Client disconnected\n");
        CLOSESOCKET(s_client_sock);
        s_client_sock = SOCKET_INVALID;
        s_paused = 0;
    }
#ifdef _WIN32
    else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        CLOSESOCKET(s_client_sock);
        s_client_sock = SOCKET_INVALID;
        s_paused = 0;
    }
#else
    else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        CLOSESOCKET(s_client_sock);
        s_client_sock = SOCKET_INVALID;
        s_paused = 0;
    }
#endif
}

// Internal poll function called by the network thread.
// Must hold the mutex when accessing shared state.
static void debug_server_poll_internal(void) {
    // Accept new connections
    if (s_client_sock == SOCKET_INVALID && s_listen_sock != SOCKET_INVALID) {
        s_client_sock = accept(s_listen_sock, NULL, NULL);
        if (s_client_sock != SOCKET_INVALID) {
            set_nonblocking(s_client_sock);
            s_recv_len = 0;
            fprintf(stderr, "[debug_server] Client connected\n");
            send_fmt("{\"connected\":true,\"frame\":%d}", snes_frame_counter);
        }
    }

    // Check watchpoints and address trace (reads s_ram)
    lock_mutex();
    check_watchpoints();
    check_addr_trace();
    check_range_trace();
    unlock_mutex();

    // Process commands (command handlers read shared state)
    lock_mutex();
    try_recv_and_process();
    unlock_mutex();
}

// Background thread entry point: loops poll + sleep until shutdown.
#ifdef _WIN32
static unsigned __stdcall debug_server_thread(void *arg) {
    (void)arg;
    while (!s_shutdown) {
        debug_server_poll_internal();
        Sleep(5);  // 5ms — responsive enough for debug queries
    }
    return 0;
}
#else
static void *debug_server_thread(void *arg) {
    (void)arg;
    while (!s_shutdown) {
        debug_server_poll_internal();
        usleep(5000);
    }
    return NULL;
}
#endif

void debug_server_start_paused(void) {
    s_paused = 1;
}

void debug_server_wait_if_paused(void) {
    while (s_paused) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
}

int debug_server_consume_loadstate(void) {
    int slot = s_pending_loadstate;
    if (slot >= 0)
        s_pending_loadstate = -1;
    return slot;
}

uint32_t debug_server_get_controller_inputs(void) {
    return s_controller_inputs;
}

uint32_t debug_server_get_controller_active_mask(void) {
    return (s_controller_active & 0x3u) << 30;
}

// Legacy poll — now a no-op since the background thread handles networking.
void debug_server_poll(void) {
    // No-op: networking is handled by the background thread.
    // Kept for API compatibility.
}

void debug_server_shutdown(void) {
    // Signal thread to stop
    s_shutdown = 1;

    // Wait for the network thread to exit
#ifdef _WIN32
    if (s_thread) {
        WaitForSingleObject(s_thread, 2000);  // 2s timeout
        CloseHandle(s_thread);
        s_thread = NULL;
    }
#else
    if (s_thread_created) {
        pthread_join(s_thread, NULL);
        s_thread_created = 0;
    }
#endif

    if (s_client_sock != SOCKET_INVALID) CLOSESOCKET(s_client_sock);
    if (s_listen_sock != SOCKET_INVALID) CLOSESOCKET(s_listen_sock);
    s_client_sock = SOCKET_INVALID;
    s_listen_sock = SOCKET_INVALID;

#ifdef _WIN32
    DeleteCriticalSection(&s_mutex);
    WSACleanup();
#endif
}
