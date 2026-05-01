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

// ---- VRAM-word write trace ----
// Captures every word-address write to PPU VRAM with attribution. Unlike
// reg trace this sees *all* writes, including LoadStripeImage_UploadToVRAM
// and other hand-code that bypasses $2118/$2119 and writes g_ppu->vram
// directly. Enabled via "trace_vram <lo> <hi>" (word addresses, up to
// MAX_VRAM_TRACE_RANGES disjoint ranges); read via "get_vram_trace";
// cleared via "trace_vram_reset".
#define VRAM_TRACE_LOG_SIZE 65536
#define MAX_VRAM_TRACE_RANGES 8
static struct {
    int active;
    int nranges;
    struct { uint16_t lo, hi; } ranges[MAX_VRAM_TRACE_RANGES];
    int write_idx;
    int count;
    struct {
        int frame;
        uint16_t adr;
        uint16_t val;
        char func[64];
        const char *stack[TRACE_STACK_DEPTH];
        int stack_depth;
    } log[VRAM_TRACE_LOG_SIZE];
} s_vram_trace = {0};

void debug_server_on_vram_write(uint16_t adr_word, uint16_t value) {
    if (!s_vram_trace.active) return;
    int hit = 0;
    for (int i = 0; i < s_vram_trace.nranges; i++)
        if (adr_word >= s_vram_trace.ranges[i].lo &&
            adr_word <= s_vram_trace.ranges[i].hi) { hit = 1; break; }
    if (!hit) return;
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    int idx = s_vram_trace.write_idx % VRAM_TRACE_LOG_SIZE;
    s_vram_trace.log[idx].frame = snes_frame_counter;
    s_vram_trace.log[idx].adr = adr_word;
    s_vram_trace.log[idx].val = value;
    if (g_last_recomp_func)
        strncpy(s_vram_trace.log[idx].func, g_last_recomp_func, 63);
    else
        strcpy(s_vram_trace.log[idx].func, "(none)");
    s_vram_trace.log[idx].func[63] = 0;
    int depth = g_recomp_stack_top < TRACE_STACK_DEPTH ? g_recomp_stack_top : TRACE_STACK_DEPTH;
    s_vram_trace.log[idx].stack_depth = depth;
    for (int s = 0; s < depth; s++)
        s_vram_trace.log[idx].stack[s] = g_recomp_stack[g_recomp_stack_top - depth + s];
    s_vram_trace.write_idx++;
    if (s_vram_trace.count < VRAM_TRACE_LOG_SIZE) s_vram_trace.count++;
}

/* Expose arm-with-default-ranges so cpu_trace_arm_default_watches can
 * turn on the MMIO ring at process startup, BEFORE the first frame
 * runs. Used 2026-04-30 to capture the boot-time DMA setup that
 * uploads ROM bank $05 bytes to VRAM $7000-$8FFF — that DMA fires
 * around frame 94, before TCP attach can manually arm. */
void debug_server_arm_default_reg_trace(void) {
    s_reg_trace.nranges = 0;
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

// ---- Tier-4 reads: WRAM read trace ----
//
// Symmetric counterpart of Tier 1's write trace. Every g_ram[X]
// read in --reverse-debug generated code routes through
// debug_wram_read_byte/word; this function records to a ring when
// the trace is active and any armed range covers the address.
//
// Gated on SNESRECOMP_TIER4 (auto-set by ENABLE_ORACLE_BACKEND).
// Production Release|x64 omits the ring + helpers entirely; the
// RDB_LOAD8/16 macros expand to direct array access there.
#if SNESRECOMP_TIER4
#define READ_TRACE_LOG_SIZE 1048576   /* 1M entries; ~80 MB */
#define READ_TRACE_MAX_RANGES 8

static volatile int s_read_trace_active = 0;
static int s_read_trace_nranges = 0;
static struct { uint32_t lo, hi; } s_read_trace_ranges[READ_TRACE_MAX_RANGES];
static int s_read_trace_write_idx = 0;
static int s_read_trace_count = 0;
static struct {
    int      frame;
    uint64_t block_idx;
    uint32_t adr;
    uint16_t val;
    uint8_t  width;
    char     func[48];
    char     parent[48];
} s_read_trace[READ_TRACE_LOG_SIZE];

extern volatile uint64_t g_block_counter;

static inline int read_trace_in_range(uint32_t adr, uint8_t width) {
    uint32_t hi_byte = adr + width - 1;
    for (int i = 0; i < s_read_trace_nranges; i++)
        if (adr <= s_read_trace_ranges[i].hi && hi_byte >= s_read_trace_ranges[i].lo)
            return 1;
    return 0;
}

static void read_trace_record(uint32_t adr, uint16_t val, uint8_t width) {
    if (!s_read_trace_active) return;
    if (!read_trace_in_range(adr, width)) return;
    int idx = s_read_trace_write_idx % READ_TRACE_LOG_SIZE;
    s_read_trace[idx].frame = snes_frame_counter;
    s_read_trace[idx].block_idx = g_block_counter;
    s_read_trace[idx].adr = adr;
    s_read_trace[idx].val = val;
    s_read_trace[idx].width = width;
    if (g_last_recomp_func) {
        strncpy(s_read_trace[idx].func, g_last_recomp_func, 47);
        s_read_trace[idx].func[47] = 0;
    } else {
        s_read_trace[idx].func[0] = 0;
    }
    s_read_trace[idx].parent[0] = 0;
    if (g_recomp_stack_top >= 2) {
        const char *p = g_recomp_stack[g_recomp_stack_top - 2];
        if (p) {
            strncpy(s_read_trace[idx].parent, p, 47);
            s_read_trace[idx].parent[47] = 0;
        }
    }
    s_read_trace_write_idx++;
    if (s_read_trace_count < READ_TRACE_LOG_SIZE) s_read_trace_count++;
}

uint8_t debug_wram_read_byte(uint32_t addr) {
    uint8_t v = g_ram[addr];
    read_trace_record(addr, v, 1);
    return v;
}

uint16_t debug_wram_read_word(uint32_t addr) {
    uint16_t v = *(uint16_t *)(g_ram + addr);
    read_trace_record(addr, v, 2);
    return v;
}
#endif /* SNESRECOMP_TIER4 */

// ---- Tier-4 per-instruction trace ----
//
// Captures every 65816 instruction the recomp executes. Disabled by
// default. When trace_insn arms it, every instruction-emit-time call
// to debug_on_insn_enter records (frame, block_idx, pc, mnem_id) into
// a 2M-entry ring (~24 MB). Memory budget chosen so the ring covers
// roughly tens of seconds of attract-demo at typical instruction
// rates.
//
// The mnemonic table is shared with all entries; the entry stores a
// small int index. Probes can fetch the table via get_insn_mnemonics.
//
// Entire subsystem gated on SNESRECOMP_TIER4 (debug_server.h sets
// this when ENABLE_ORACLE_BACKEND is defined). Production Release|x64
// leaves it undefined; the ring and TCP commands are entirely absent
// from the binary.
#if SNESRECOMP_TIER4
#define INSN_TRACE_LOG_SIZE 2097152
static volatile int s_insn_trace_active = 0;
static int s_insn_trace_write_idx = 0;
static int s_insn_trace_count = 0;
static struct {
    int      frame;
    uint64_t block_idx;
    uint32_t pc;
    uint32_t mnem_id;
    uint32_t a, x, y, b;     // RDB_REG_UNKNOWN (0xFFFFFFFF) when not pinned
    uint8_t  m_flag;         // 1 if 8-bit accumulator
    uint8_t  x_flag;         // 1 if 8-bit index
} s_insn_trace[INSN_TRACE_LOG_SIZE];

// Forward declaration: g_block_counter is defined below in the Tier 3
// section but referenced here. Both live in this same TU so the
// extern is just for forward-visibility within the file.
extern volatile uint64_t g_block_counter;

void debug_on_insn_enter(uint32_t pc, uint32_t mnem_id,
                         uint32_t a, uint32_t x, uint32_t y, uint32_t b,
                         uint32_t mx_flags) {
    if (!s_insn_trace_active) return;
    int idx = s_insn_trace_write_idx % INSN_TRACE_LOG_SIZE;
    s_insn_trace[idx].frame = snes_frame_counter;
    s_insn_trace[idx].block_idx = g_block_counter;
    s_insn_trace[idx].pc = pc;
    s_insn_trace[idx].mnem_id = mnem_id;
    s_insn_trace[idx].a = a;
    s_insn_trace[idx].x = x;
    s_insn_trace[idx].y = y;
    s_insn_trace[idx].b = b;
    s_insn_trace[idx].m_flag = (uint8_t)((mx_flags >> 0) & 1);
    s_insn_trace[idx].x_flag = (uint8_t)((mx_flags >> 1) & 1);
    s_insn_trace_write_idx++;
    if (s_insn_trace_count < INSN_TRACE_LOG_SIZE) s_insn_trace_count++;
}
#endif /* SNESRECOMP_TIER4 */

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
#define CALL_TRACE_LOG_SIZE 65536
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
} s_call_trace = {0};

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

    // APU pacing: bump the main-CPU cycle estimate. Average ~24 cycles
    // per basic block (each block is typically 3-5 65816 instructions
    // averaging ~6 cycles each). RtlApuWrite / snes_readBBus use this
    // counter to drive realistic snes_catchupApu() so SMW's "wait for
    // APU ack" loops actually wait. Plain unsigned add, no branch.
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
    s_paused = 1;
    send_fmt("{\"ok\":true,\"paused\":true,\"frame\":%d}", snes_frame_counter);
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
    /* BLOCK until the main loop has run the requested frames and re-paused.
     * Caps at ~5 s of wall-clock wait (150k × 30 µs) so a stuck main loop
     * doesn't wedge the network thread forever — if the timeout hits,
     * respond with what we have and let the caller diagnose. */
    int waited = 0;
    while (s_step_remaining > 0 && waited < 150000) {
#ifdef _WIN32
        Sleep(0);
#else
        struct timespec ts = {0, 30000};  /* 30 µs */
        nanosleep(&ts, NULL);
#endif
        waited++;
    }
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
        "],\"entries\":%d,\"log\":[", s_vram_trace.count);
    int start = s_vram_trace.count < VRAM_TRACE_LOG_SIZE ? 0 :
                s_vram_trace.write_idx - VRAM_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 4096;
    for (int i = 0; i < s_vram_trace.count && pos < budget; i++) {
        int idx = (start + i) % VRAM_TRACE_LOG_SIZE;
        if (nostack) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"f\":%d,\"adr\":\"0x%04x\",\"val\":\"0x%04x\",\"func\":\"%s\"}",
                i ? "," : "",
                s_vram_trace.log[idx].frame,
                s_vram_trace.log[idx].adr,
                s_vram_trace.log[idx].val,
                s_vram_trace.log[idx].func);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"f\":%d,\"adr\":\"0x%04x\",\"val\":\"0x%04x\",\"func\":\"%s\",\"stack\":[",
                i ? "," : "",
                s_vram_trace.log[idx].frame,
                s_vram_trace.log[idx].adr,
                s_vram_trace.log[idx].val,
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

#if SNESRECOMP_TIER4
// ---- Tier-4: WRAM-read trace TCP commands ----
// trace_wram_reads <hex_lo> [hex_hi] : add a watched range, arm trace
// trace_wram_reads_reset             : drop all ranges, clear ring
// get_wram_read_trace [filters]      : dump ring as JSON

static void cmd_trace_wram_reads(const char *args) {
    unsigned int lo = 0, hi = 0;
    int n = args ? sscanf(args, "%x %x", &lo, &hi) : 0;
    if (n < 1) {
        send_fmt("{\"error\":\"usage: trace_wram_reads <hex_lo> [hex_hi]\"}"); return;
    }
    if (n < 2) hi = lo;
    if (hi < lo || hi > 0x1FFFF) {
        send_fmt("{\"error\":\"bad range\"}"); return;
    }
    if (s_read_trace_nranges >= READ_TRACE_MAX_RANGES) {
        send_fmt("{\"error\":\"too many ranges (max %d)\"}", READ_TRACE_MAX_RANGES); return;
    }
    s_read_trace_ranges[s_read_trace_nranges].lo = lo;
    s_read_trace_ranges[s_read_trace_nranges].hi = hi;
    s_read_trace_nranges++;
    s_read_trace_active = 1;
    send_fmt("{\"ok\":true,\"lo\":\"0x%05x\",\"hi\":\"0x%05x\",\"nranges\":%d}",
             lo, hi, s_read_trace_nranges);
}

static void cmd_trace_wram_reads_reset(const char *args) {
    (void)args;
    s_read_trace_active = 0;
    s_read_trace_nranges = 0;
    s_read_trace_write_idx = 0;
    s_read_trace_count = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_get_wram_read_trace(const char *args) {
    (void)args;
    static char buf[524288];
    int pos = snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%d,\"log\":[",
                       s_read_trace_count);
    int start = s_read_trace_count < READ_TRACE_LOG_SIZE ? 0 :
                s_read_trace_write_idx - READ_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 256;
    for (int i = 0; i < s_read_trace_count && pos < budget; i++) {
        int idx = (start + i) % READ_TRACE_LOG_SIZE;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"bi\":%llu,\"adr\":\"0x%05x\",\"val\":\"0x%04x\","
            "\"w\":%u,\"func\":\"%s\",\"parent\":\"%s\"}",
            i ? "," : "",
            s_read_trace[idx].frame,
            (unsigned long long)s_read_trace[idx].block_idx,
            s_read_trace[idx].adr,
            s_read_trace[idx].val,
            (unsigned)s_read_trace[idx].width,
            s_read_trace[idx].func,
            s_read_trace[idx].parent);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

// ---- Tier-4: per-instruction trace TCP commands ----
//
// trace_insn / trace_insn_reset / get_insn_trace mirror the
// trace_blocks family. Insn entries are tiny (no func name) so
// 2M-entry ring fits ~24 MB; cap on get_insn_trace JSON response
// is 256 entries unless filter narrows further.
//
// Filters: from_block / to_block (block_idx range), pc_lo / pc_hi.
static void cmd_trace_insn(const char *args) {
    (void)args;
    s_insn_trace_active = 1;
    send_fmt("{\"ok\":true,\"max_entries\":%d}", INSN_TRACE_LOG_SIZE);
}

static void cmd_trace_insn_reset(const char *args) {
    (void)args;
    s_insn_trace_active = 0;
    s_insn_trace_write_idx = 0;
    s_insn_trace_count = 0;
    send_fmt("{\"ok\":true}");
}

static void cmd_get_insn_trace(const char *args) {
    unsigned long long from_bi = 0, to_bi = 0xFFFFFFFFFFFFFFFFull;
    uint32_t pc_lo = 0, pc_hi = 0xFFFFFFu;
    int max_emit = 256;
    if (args) {
        const char *p;
        if ((p = strstr(args, "from_block="))) sscanf(p + 11, "%llu", &from_bi);
        if ((p = strstr(args, "to_block=")))   sscanf(p + 9,  "%llu", &to_bi);
        if ((p = strstr(args, "pc_lo=")))      sscanf(p + 6,  "%x", &pc_lo);
        if ((p = strstr(args, "pc_hi=")))      sscanf(p + 6,  "%x", &pc_hi);
        if ((p = strstr(args, "limit=")))      sscanf(p + 6,  "%d", &max_emit);
    }
    if (max_emit < 1) max_emit = 1;
    if (max_emit > 4096) max_emit = 4096;

    static char buf[262144];
    int pos = snprintf(buf, sizeof(buf), "{\"ok\":true,\"total\":%d,\"log\":[",
                       s_insn_trace_count);
    int start = s_insn_trace_count < INSN_TRACE_LOG_SIZE ? 0 :
                s_insn_trace_write_idx - INSN_TRACE_LOG_SIZE;
    int budget = (int)sizeof(buf) - 256;
    int emitted = 0;
    int first = 1;
    for (int i = 0; i < s_insn_trace_count && pos < budget && emitted < max_emit; i++) {
        int idx = (start + i) % INSN_TRACE_LOG_SIZE;
        uint64_t bi = s_insn_trace[idx].block_idx;
        uint32_t pc = s_insn_trace[idx].pc;
        if (bi < from_bi || bi > to_bi) continue;
        if (pc < pc_lo || pc > pc_hi) continue;
        // Emit register values as hex strings, "?" when not pinned.
        char a_buf[12], x_buf[12], y_buf[12], b_buf[12];
        #define _FMT_REG(buf, val) do { \
            if ((val) == 0xFFFFFFFFu) snprintf(buf, sizeof(buf), "\"?\""); \
            else snprintf(buf, sizeof(buf), "\"0x%04x\"", (val) & 0xFFFFu); \
        } while(0)
        _FMT_REG(a_buf, s_insn_trace[idx].a);
        _FMT_REG(x_buf, s_insn_trace[idx].x);
        _FMT_REG(y_buf, s_insn_trace[idx].y);
        _FMT_REG(b_buf, s_insn_trace[idx].b);
        #undef _FMT_REG
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"bi\":%llu,\"pc\":\"0x%06x\",\"mnem\":%u,"
            "\"a\":%s,\"x\":%s,\"y\":%s,\"b\":%s,\"m\":%u,\"xf\":%u}",
            first ? "" : ",",
            s_insn_trace[idx].frame,
            (unsigned long long)bi, pc,
            s_insn_trace[idx].mnem_id,
            a_buf, x_buf, y_buf, b_buf,
            (unsigned)s_insn_trace[idx].m_flag,
            (unsigned)s_insn_trace[idx].x_flag);
        first = 0;
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", emitted);
    send_line(buf);
}

// Mnemonic table: indices used in the insn ring map to these strings.
// Recomp.py emits the same indices (kept in sync via the test
// test_insn_hook_mnemonic_table_in_sync).
static const char *const s_insn_mnemonics[] = {
    "?",   "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE",
    "BPL", "BRA", "BRK", "BRL", "BVC", "BVS", "CLC", "CLD", "CLI", "CLV",
    "CMP", "COP", "CPX", "CPY", "DEC", "DEX", "DEY", "EOR", "INC", "INX",
    "INY", "JMP", "JML", "JSL", "JSR", "LDA", "LDX", "LDY", "LSR", "MVN",
    "MVP", "NOP", "ORA", "PEA", "PEI", "PER", "PHA", "PHB", "PHD", "PHK",
    "PHP", "PHX", "PHY", "PLA", "PLB", "PLD", "PLP", "PLX", "PLY", "REP",
    "ROL", "ROR", "RTI", "RTL", "RTS", "SBC", "SEC", "SED", "SEI", "SEP",
    "STA", "STP", "STX", "STY", "STZ", "TAX", "TAY", "TCD", "TCS", "TDC",
    "TRB", "TSB", "TSC", "TSX", "TXA", "TXS", "TXY", "TYA", "TYX", "WAI",
    "WDM", "XBA", "XCE",
};
#define INSN_MNEM_COUNT ((int)(sizeof(s_insn_mnemonics) / sizeof(s_insn_mnemonics[0])))

static void cmd_get_insn_mnemonics(const char *args) {
    (void)args;
    static char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%d,\"table\":[",
                       INSN_MNEM_COUNT);
    for (int i = 0; i < INSN_MNEM_COUNT; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s\"%s\"", i ? "," : "", s_insn_mnemonics[i]);
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

#endif /* SNESRECOMP_TIER4 */

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

    // Render current PPU state into a temp buffer
    static uint8_t scr_pixels[256 * 4 * 240];
    PpuBeginDrawing(g_ppu, scr_pixels, 256 * 4, 0);

    // Run HDMA + scanlines like SmwDrawPpuFrame but without IRQ
    for (int i = 0; i <= 224; i++)
        ppu_runLine(g_ppu, i);

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
             g_cpu.A, g_cpu.B, g_cpu.X, g_cpu.Y,
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

/* Persistent BBAA injection: every 16-bit read of $2140 returns
 * $BBAA when set. Helps isolate whether boot is hung on engine-not-
 * writing vs host-poll-broken. */
extern int g_force_apu_bbaa;
extern int g_apu_autoack;
static void cmd_force_apu_bbaa(const char *args) {
    int v = 1;
    sscanf(args ? args : "", "%d", &v);
    g_force_apu_bbaa = v;
    send_fmt("{\"ok\":true,\"force\":%d}", g_force_apu_bbaa);
}
static void cmd_apu_autoack(const char *args) {
    int v = 1;
    sscanf(args ? args : "", "%d", &v);
    g_apu_autoack = v;
    send_fmt("{\"ok\":true,\"autoack\":%d}", g_apu_autoack);
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
 *     before_idx=N        — start scan from absolute idx N-1 (default = g_cpu_trace_idx)
 *
 * Each emitted event is JSON: idx, event, pc24, A,X,Y,S,D, DB,PB,P,m,x,
 * extra0, extra1.
 */
static void cmd_trace_get_v2(const char *args) {
#if SNESRECOMP_TRACE
    int count = 64;
    int skip = 0;
    int event_filter = -1;
    int bank_filter = -1;
    unsigned int addr_lo = 0, addr_hi = 0xFFFF;
    long long before_idx_arg = -1;
    if (args) {
        const char *p;
        if ((p = strstr(args, "count=")) != NULL) sscanf(p + 6, "%d", &count);
        if ((p = strstr(args, "skip=")) != NULL) sscanf(p + 5, "%d", &skip);
        if ((p = strstr(args, "event=")) != NULL) sscanf(p + 6, "%d", &event_filter);
        if ((p = strstr(args, "bank=")) != NULL) { unsigned int b = 0; if (sscanf(p + 5, "%x", &b) == 1) bank_filter = (int)b; }
        if ((p = strstr(args, "addr_lo=")) != NULL) sscanf(p + 8, "%x", &addr_lo);
        if ((p = strstr(args, "addr_hi=")) != NULL) sscanf(p + 8, "%x", &addr_hi);
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
    int max_scan = CPU_TRACE_RING_LEN;
    for (int i = 1; i <= max_scan && emitted < count; i++) {
        if ((uint64_t)i > end_idx) break;
        uint64_t abs_idx = end_idx - i;
        int slot = (int)(abs_idx & (CPU_TRACE_RING_LEN - 1));
        CpuTraceEvent *e = &g_cpu_trace_ring[slot];
        /* Skip if filter mismatches. */
        if (event_filter >= 0 && e->event_type != (uint8_t)event_filter) continue;
        if (e->event_type == CPU_TR_WRAM_WRITE) {
            uint8_t b = (uint8_t)(e->extra1 >> 8);
            uint16_t a = (uint16_t)(e->pc24 & 0xFFFF);  /* extra1 hit-byte; pc24 has bank, not addr */
            /* WRAM_WRITE pc24 is (PB<<16); the actual write address isn't
             * preserved per-event. The cpu_trace path stores hit_byte_in_word
             * in extra1's low byte but not the addr. For range filtering on
             * WRAM_WRITE, use the most recent BLOCK event's PC as
             * approximation OR rely on event_type filter only.
             *
             * For now: bank filter applies; addr range filter skipped on
             * WRAM_WRITE because the addr isn't stored. Clients should use
             * the scoped tripwire (trip_get) for write-localized capture. */
            (void)a;
            if (bank_filter >= 0 && b != (uint8_t)bank_filter) continue;
            (void)addr_lo; (void)addr_hi;
        }
        if (skip > 0 && seen_matching < skip) { seen_matching++; continue; }
        seen_matching++;

        if (pos > (int)sizeof(buf) - 512) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"idx\":%llu,\"event\":%u,\"pc24\":\"0x%06x\","
            "\"A\":\"0x%04x\",\"X\":\"0x%04x\",\"Y\":\"0x%04x\","
            "\"S\":\"0x%04x\",\"D\":\"0x%04x\","
            "\"DB\":\"0x%02x\",\"PB\":\"0x%02x\",\"P\":\"0x%02x\","
            "\"m\":%u,\"x\":%u,"
            "\"extra0\":\"0x%02x\",\"extra1\":\"0x%04x\","
            "\"hash\":\"0x%08x\"}",
            emitted ? "," : "",
            (unsigned long long)abs_idx,
            (unsigned)e->event_type, e->pc24,
            e->A, e->X, e->Y, e->S, e->D,
            e->DB, e->PB, e->P, e->M, e->XF,
            e->extra0, e->extra1,
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

/* Post-mortem JSON dump — calls into smw_post_mortem_dump (in src/post_mortem.c).
 * Declared weakly so non-SMW projects sharing this debug_server.c don't
 * fail to link; if no implementation is present, the command just emits
 * a sentinel reply. */
extern void smw_post_mortem_dump(const char *reason, void *fault_info);
static void cmd_post_mortem_dump(const char *args) {
    (void)args;
    smw_post_mortem_dump("on_demand", NULL);
    send_line("{\"ok\":true,\"path\":\"build/last_run_report.json\"}");
}

typedef struct { const char *name; void (*handler)(const char *args); } CmdEntry;
static const CmdEntry s_commands[] = {
    {"ping",          cmd_ping},
    {"post_mortem_dump", cmd_post_mortem_dump},
    {"get_v2_cpu",    cmd_get_v2_cpu},
    {"force_apu_bbaa", cmd_force_apu_bbaa},
    {"apu_autoack",    cmd_apu_autoack},
    {"trace_dump",     cmd_trace_dump},
    {"trace_dbpb",     cmd_trace_dbpb},
    {"trace_clear",    cmd_trace_clear},
    {"trace_get_v2",   cmd_trace_get_v2},
    {"tripwire_arm",   cmd_tripwire_arm},
    {"tripwire_get",   cmd_tripwire_get},
    {"tripwire_disarm", cmd_tripwire_disarm},
    {"dma_trip_get",   cmd_dma_trip_get},
    {"pxwatch_arm",    cmd_pxwatch_arm},
    {"pxwatch_disarm", cmd_pxwatch_disarm},
    {"pxwatch_clear",  cmd_pxwatch_clear},
    {"pxwatch_get",    cmd_pxwatch_get},
    {"set_db_watch",   cmd_set_db_watch},
    {"arm_watches",    cmd_arm_watches},
    {"set_wram_watch", cmd_set_wram_watch},
    {"clear_wram_watches", cmd_clear_wram_watches},
    {"trace_dump_wram", cmd_trace_dump_wram},
    {"get_spc_writes", cmd_get_spc_writes},
    {"get_spc_pc_hist", cmd_get_spc_pc_hist},
    {"get_apu_misc",   cmd_get_apu_misc},
    {"frame",         cmd_frame},
    {"read_ram",      cmd_read_ram},
    {"dump_ram",      cmd_dump_ram},
    {"call_stack",    cmd_call_stack},
    {"watch",         cmd_watch},
    {"unwatch",       cmd_unwatch},
    {"pause",         cmd_pause},
    {"continue",      cmd_continue},
    {"step",          cmd_step},
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
#if SNESRECOMP_TIER4
    {"trace_insn",            cmd_trace_insn},
    {"trace_insn_reset",      cmd_trace_insn_reset},
    {"get_insn_trace",        cmd_get_insn_trace},
    {"get_insn_mnemonics",    cmd_get_insn_mnemonics},
    {"trace_wram_reads",      cmd_trace_wram_reads},
    {"trace_wram_reads_reset",cmd_trace_wram_reads_reset},
    {"get_wram_read_trace",   cmd_get_wram_read_trace},
#endif
#endif
    {"trace_range",   cmd_trace_range},
    {"get_trace_range", cmd_get_trace_range},
    {"get_frame",     cmd_get_frame},
    {"frame_range",   cmd_frame_range},
    {"history",       cmd_history_status},
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

#ifdef ENABLE_ORACLE_BACKEND
    // Pre-dispatch hook for the emulator-oracle family (emu_*). Keeps
    // backend-specific code out of this file.
    extern int emu_oracle_handle_cmd(const char *cmd, const char *args);
    if (emu_oracle_handle_cmd(line, args)) return;
#endif

    for (const CmdEntry *c = s_commands; c->name; c++) {
        if (strcmp(line, c->name) == 0) {
            c->handler(args);
            return;
        }
    }
    send_fmt("{\"error\":\"unknown command\",\"cmd\":\"%s\"}", line);
}

#ifdef ENABLE_ORACLE_BACKEND
// Non-static wrappers exposed for emu_oracle_cmds.c (another TU).
// Preserves the "static send_line/send_fmt" internal convention for
// the rest of this file while letting oracle handlers reuse the same
// buffer + socket + newline framing.
void debug_server_send_line(const char *line) { send_line(line); }
void debug_server_send_fmt(const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_line(buf);
}
// Raw byte send — no newline framing — for streaming large hex payloads
// (the dump_ram / read_ram / find_first_divergence pattern). Caller is
// responsible for any framing.
void debug_server_send_raw(const void *data, int len) {
    if (s_client_sock == SOCKET_INVALID) return;
    send(s_client_sock, (const char *)data, len, 0);
}
#endif

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

    // Always-on VRAM trace: capture every word-VRAM write continuously
    // so probes can attribute Layer-3 / OBJ tile-graphics corruption
    // backward in history. Same rationale as WRAM. Ring covers the full
    // word-address range $0000-$FFFF; VRAM_TRACE_LOG_SIZE = 65536 ×
    // ~140B ≈ 9 MB resident. Gated on SNESRECOMP_REVERSE_DEBUG.
    s_vram_trace.nranges = 1;
    s_vram_trace.ranges[0].lo = 0x0000;
    s_vram_trace.ranges[0].hi = 0xFFFF;
    s_vram_trace.active = 1;
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
