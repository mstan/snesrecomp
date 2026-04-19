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
extern Ppu *g_ppu;
extern Cpu *g_cpu;
extern Dma *g_dma;
extern Snes *g_snes;

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

// ---- Map16 write log ----
// Captures every write to the Map16 low-byte range ($C800-$CFFF) with full context.
// Enabled via TCP command "map16_trace_on", retrieved via "get_map16_trace".
#define MAP16_LOG_SIZE 1024
static struct {
    int active;
    int write_idx;
    int count;
    struct {
        int frame;
        uint16_t ram_addr;    // effective RAM address written
        uint8_t value;        // value written
        uint16_t ptr_lo;      // 3-byte pointer at $6B (low 16 bits)
        uint8_t ptr_bank;     // bank byte at $6D
        uint16_t offset;      // Y offset passed to IndirWriteByte
        uint8_t dp57;         // object Y position register
        uint8_t dp0a;         // level data byte 0A
        uint8_t dp0b;         // level data byte 0B
        uint16_t dp65;        // level data stream pointer ($65/$66)
        uint8_t dp67;         // level data bank byte ($67)
        char func[64];
        const char *stack[TRACE_STACK_DEPTH];
        int stack_depth;
    } log[MAP16_LOG_SIZE];
} s_map16_trace = {0};

void debug_server_log_map16_write(uint16_t ram_addr, uint8_t value,
                                   uint16_t ptr_lo, uint8_t ptr_bank,
                                   uint16_t offset) {
    if (!s_map16_trace.active) return;
    // Only log writes to Map16 low-byte range $C800-$CFFF
    if (ram_addr < 0xC800 || ram_addr > 0xCFFF) return;

    int idx = s_map16_trace.write_idx % MAP16_LOG_SIZE;
    s_map16_trace.log[idx].frame = snes_frame_counter;
    s_map16_trace.log[idx].ram_addr = ram_addr;
    s_map16_trace.log[idx].value = value;
    s_map16_trace.log[idx].ptr_lo = ptr_lo;
    s_map16_trace.log[idx].ptr_bank = ptr_bank;
    s_map16_trace.log[idx].offset = offset;
    s_map16_trace.log[idx].dp57 = s_ram ? s_ram[0x57] : 0;
    s_map16_trace.log[idx].dp0a = s_ram ? s_ram[0x0a] : 0;
    s_map16_trace.log[idx].dp0b = s_ram ? s_ram[0x0b] : 0;
    s_map16_trace.log[idx].dp65 = s_ram ? (s_ram[0x65] | (s_ram[0x66] << 8)) : 0;
    s_map16_trace.log[idx].dp67 = s_ram ? s_ram[0x67] : 0;
    if (g_last_recomp_func)
        strncpy(s_map16_trace.log[idx].func, g_last_recomp_func, 63);
    else
        strcpy(s_map16_trace.log[idx].func, "(none)");
    s_map16_trace.log[idx].func[63] = 0;
    int depth = g_recomp_stack_top < TRACE_STACK_DEPTH ? g_recomp_stack_top : TRACE_STACK_DEPTH;
    s_map16_trace.log[idx].stack_depth = depth;
    for (int s = 0; s < depth; s++)
        s_map16_trace.log[idx].stack[s] = g_recomp_stack[g_recomp_stack_top - depth + s];
    s_map16_trace.write_idx++;
    if (s_map16_trace.count < MAP16_LOG_SIZE) s_map16_trace.count++;
}

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

void debug_server_profile_frame_start(void) {
    if (!s_profile_enabled || s_profile_latched) return;
    s_profile_count = 0;
    s_profile_frame_start = clock();
}

void debug_server_profile_frame_end(void) {
    if (!s_profile_enabled || s_profile_latched) return;
    s_profile_frame_ms = (double)(clock() - s_profile_frame_start) * 1000.0 / CLOCKS_PER_SEC;
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

// Per-frame DMA channel snapshot (8 bytes per channel)
typedef struct {
    uint8_t bAdr, aBank, mode, flags; // flags: bit0=dmaActive,1=hdmaActive,2=fixed,3=decrement,4=indirect,5=fromB
    uint16_t aAdr, size;
} FrameDmaChannelSnap;

typedef struct {
    int frame_number;
    char last_func[64];
    uint8_t snap[SNAP_BYTES]; // key game state snapshot for cross-server comparison
    // --- Extended state (added for exhaustive comparison) ---
    FrameCpuSnap cpu;
    FramePpuSnap ppu;
    FrameDmaChannelSnap dma[8];
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
    if (g_cpu) {
        r->cpu.a = g_cpu->a;
        r->cpu.x = g_cpu->x;
        r->cpu.y = g_cpu->y;
        r->cpu.sp = g_cpu->sp;
        r->cpu.pc = g_cpu->pc;
        r->cpu.dp = g_cpu->dp;
        r->cpu.k = g_cpu->k;
        r->cpu.db = g_cpu->db;
        r->cpu.flags = (g_cpu->c ? 1 : 0) | (g_cpu->z ? 2 : 0) | (g_cpu->v ? 4 : 0) |
                        (g_cpu->n ? 8 : 0) | (g_cpu->i ? 16 : 0) | (g_cpu->d ? 32 : 0) |
                        (g_cpu->xf ? 64 : 0) | (g_cpu->mf ? 128 : 0);
        r->cpu.e = g_cpu->e ? 1 : 0;
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

    // DMA channels
    if (g_dma) {
        for (int ch = 0; ch < 8; ch++) {
            DmaChannel *dc = &g_dma->channel[ch];
            r->dma[ch].bAdr = dc->bAdr;
            r->dma[ch].aBank = dc->aBank;
            r->dma[ch].mode = dc->mode;
            r->dma[ch].flags = (dc->dmaActive ? 1 : 0) | (dc->hdmaActive ? 2 : 0) |
                                (dc->fixed ? 4 : 0) | (dc->decrement ? 8 : 0) |
                                (dc->indirect ? 16 : 0) | (dc->fromB ? 32 : 0);
            r->dma[ch].aAdr = dc->aAdr;
            r->dma[ch].size = dc->size;
        }
    } else {
        memset(r->dma, 0, sizeof(r->dma));
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

static void cmd_read_ram(const char *args) {
    unsigned int addr = 0, len = 16;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 1024) len = 1024;
    if (!s_ram || addr + len > s_ram_size) {
        send_fmt("{\"error\":\"out of range\",\"addr\":\"0x%x\",\"max\":\"0x%x\"}", addr, s_ram_size);
        return;
    }
    // Build hex string
    char hex[4096];
    int pos = 0;
    for (unsigned int i = 0; i < len && pos < 4000; i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%s%02x", i ? " " : "", s_ram[addr + i]);
    send_fmt("{\"addr\":\"0x%x\",\"len\":%u,\"hex\":\"%s\"}", addr, len, hex);
}

// dump_ram: large-range hex dump for oracle comparison.
// Usage: dump_ram <start_hex> <len_decimal>
// Returns hex bytes as a long string (up to 64KB).
static void cmd_dump_ram(const char *args) {
    unsigned int addr = 0, len = 256;
    sscanf(args, "%x %u", &addr, &len);
    if (len > 0x10000) len = 0x10000;  // 64KB max
    if (!s_ram || addr + len > s_ram_size) {
        send_fmt("{\"error\":\"out of range\",\"addr\":\"0x%x\",\"len\":%u}", addr, len);
        return;
    }
    // Send in chunks to avoid buffer overflow
    // Format: {"addr":"0x...","len":...,"hex":"aabbcc..."}
    // 64KB = 128K hex chars. Too big for one JSON line. Use chunked format.
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

// ---- Per-dispatch object trace ----
// Captures g_ram key bytes before/after each bank 0D dispatch call.
// Enabled for a specific frame via: trace_dispatch <frame>
// Query results via: get_dispatch_trace

#define MAX_DISPATCH_TRACE 128
#define DISPATCH_KEY_BYTES 32  // number of key g_ram addresses to snapshot

// Key DP/WRAM addresses to capture per dispatch
static const uint16_t s_dispatch_key_addrs[DISPATCH_KEY_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,  // DP scratch / backup
    0x57, 0x59, 0x5A,                      // blocks_sub_scr_pos, size_or_type, object_number
    0x6B, 0x6C, 0x6E, 0x6F, 0x70,         // Map16 pointer bytes
    0x80, 0x98, 0x99, 0x9A, 0x9B,         // blocks misc
    0x1BA1 & 0xFF, 0x1928 & 0xFF,         // screen counters (low byte — need full addr)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0          // padding
};

typedef struct {
    int obj_number;              // blocks_object_number
    uint8_t before[DISPATCH_KEY_BYTES];
    uint8_t after[DISPATCH_KEY_BYTES];
    char func_name[48];          // last recomp func after dispatch
    uint16_t ptr_before;         // Map16 ptr C offset before
    uint16_t ptr_after;          // Map16 ptr C offset after
    uint32_t layer1_before;      // ptr_layer1_data offset from ROM start
    uint32_t layer1_after;
} DispatchTraceEntry;

static DispatchTraceEntry s_dispatch_trace[MAX_DISPATCH_TRACE];
static int s_dispatch_trace_count = 0;
static int s_dispatch_trace_frame = -1;  // set via trace_dispatch cmd, or hardcode for debugging

static void cmd_trace_dispatch(const char *args) {
    int frame = 0;
    sscanf(args, "%d", &frame);
    s_dispatch_trace_frame = frame;
    s_dispatch_trace_count = 0;
    send_fmt("{\"ok\":true,\"tracing_frame\":%d}", frame);
}

static void cmd_get_dispatch_trace(const char *args) {
    extern uint8_t g_ram[];
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"frame\":%d,\"count\":%d,\"entries\":[",
                       s_dispatch_trace_frame, s_dispatch_trace_count);
    for (int i = 0; i < s_dispatch_trace_count && pos < 7000; i++) {
        DispatchTraceEntry *e = &s_dispatch_trace[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                       "%s{\"obj\":%d,\"func\":\"%s\","
                       "\"ptr\":\"0x%04x->0x%04x\","
                       "\"sub\":\"0x%02x->0x%02x\","
                       "\"layer1\":\"0x%x->0x%x\"}",
                       i ? "," : "",
                       e->obj_number, e->func_name,
                       e->ptr_before, e->ptr_after,
                       e->before[6], e->after[6],
                       e->layer1_before, e->layer1_after);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
}

// Called from dispatch functions to record trace (main thread).
// Protected by mutex since the network thread reads dispatch trace.
void debug_dispatch_trace_before(int obj_number) {
    extern uint8_t g_ram[];
    extern const uint8_t *ptr_layer1_data;
    extern const uint8_t *g_rom;  // ROM base pointer
    if (snes_frame_counter != s_dispatch_trace_frame) return;
    if (s_dispatch_trace_count >= MAX_DISPATCH_TRACE) return;

    lock_mutex();

    DispatchTraceEntry *e = &s_dispatch_trace[s_dispatch_trace_count];
    e->obj_number = obj_number;
    e->ptr_before = (uint16_t)(g_ram[0x6b] | (g_ram[0x6c] << 8));
    e->layer1_before = (uint32_t)((uintptr_t)ptr_layer1_data & 0xFFFFFFFF);

    // Snapshot key bytes
    for (int i = 0; i < DISPATCH_KEY_BYTES; i++) {
        uint16_t a = s_dispatch_key_addrs[i];
        e->before[i] = (a < 0x20000) ? g_ram[a] : 0;
    }
    // Also capture full-address variables
    e->before[20] = g_ram[0x1BA1 & 0x1FFFF];  // blocks_screen_to_place_next_object
    e->before[21] = g_ram[0x1928 & 0x1FFFF];  // blocks_screen_to_place_current_object

    unlock_mutex();
}

void debug_dispatch_trace_after(void) {
    extern uint8_t g_ram[];
    extern const uint8_t *ptr_layer1_data;
    extern const uint8_t *g_rom;
    if (snes_frame_counter != s_dispatch_trace_frame) return;
    if (s_dispatch_trace_count >= MAX_DISPATCH_TRACE) return;

    lock_mutex();

    DispatchTraceEntry *e = &s_dispatch_trace[s_dispatch_trace_count];
    e->ptr_after = (uint16_t)(g_ram[0x6b] | (g_ram[0x6c] << 8));
    e->layer1_after = (uint32_t)((uintptr_t)ptr_layer1_data & 0xFFFFFFFF);

    // Snapshot key bytes after
    for (int i = 0; i < DISPATCH_KEY_BYTES; i++) {
        uint16_t a = s_dispatch_key_addrs[i];
        e->after[i] = (a < 0x20000) ? g_ram[a] : 0;
    }
    e->after[20] = g_ram[0x1BA1 & 0x1FFFF];
    e->after[21] = g_ram[0x1928 & 0x1FFFF];

    // Record function name
    if (g_last_recomp_func)
        strncpy(e->func_name, g_last_recomp_func, sizeof(e->func_name) - 1);
    else
        strcpy(e->func_name, "?");
    e->func_name[sizeof(e->func_name) - 1] = 0;

    s_dispatch_trace_count++;

    unlock_mutex();
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

static void cmd_step(const char *args) {
    int n = 1;
    if (args[0]) sscanf(args, "%d", &n);
    s_step_remaining = n;
    s_paused = 0;
    send_fmt("{\"ok\":true,\"stepping\":%d,\"frame\":%d}", n, snes_frame_counter);
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

static void cmd_map16_trace_on(const char *args) {
    s_map16_trace.active = 1;
    s_map16_trace.write_idx = 0;
    s_map16_trace.count = 0;
    send_fmt("{\"ok\":true,\"map16_trace\":\"on\"}");
}

static void cmd_map16_trace_off(const char *args) {
    s_map16_trace.active = 0;
    send_fmt("{\"ok\":true,\"map16_trace\":\"off\"}");
}

static void cmd_get_map16_trace(const char *args) {
    if (!s_map16_trace.count) {
        send_fmt("{\"entries\":0,\"log\":[]}");
        return;
    }
    // Use large buffer — up to 1024 entries
    static char buf[262144];
    int pos = snprintf(buf, sizeof(buf), "{\"entries\":%d,\"log\":[", s_map16_trace.count);
    int start = s_map16_trace.count < MAP16_LOG_SIZE ? 0 :
                s_map16_trace.write_idx - MAP16_LOG_SIZE;
    for (int i = 0; i < s_map16_trace.count && pos < 250000; i++) {
        int idx = (start + i) % MAP16_LOG_SIZE;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"f\":%d,\"addr\":\"0x%04x\",\"val\":\"0x%02x\","
            "\"ptr\":\"0x%02x%04x\",\"off\":%d,"
            "\"dp57\":\"0x%02x\",\"dp0a\":\"0x%02x\",\"dp0b\":\"0x%02x\",\"dp65\":\"0x%04x\",\"dp67\":\"0x%02x\","
            "\"func\":\"%s\",\"stack\":[",
            i ? "," : "",
            s_map16_trace.log[idx].frame,
            s_map16_trace.log[idx].ram_addr,
            s_map16_trace.log[idx].value,
            s_map16_trace.log[idx].ptr_bank,
            s_map16_trace.log[idx].ptr_lo,
            s_map16_trace.log[idx].offset,
            s_map16_trace.log[idx].dp57,
            s_map16_trace.log[idx].dp0a,
            s_map16_trace.log[idx].dp0b,
            s_map16_trace.log[idx].dp65,
            s_map16_trace.log[idx].dp67,
            s_map16_trace.log[idx].func);
        for (int s = 0; s < s_map16_trace.log[idx].stack_depth; s++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s\"%s\"", s ? "," : "",
                s_map16_trace.log[idx].stack[s] ? s_map16_trace.log[idx].stack[s] : "?");
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

static void cmd_get_cpu_state(const char *args) {
    if (!g_cpu) { send_fmt("{\"error\":\"cpu not available\"}"); return; }
    Cpu *c = g_cpu;
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
    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "{\"channels\":[");
    for (int ch = 0; ch < 8; ch++) {
        DmaChannel *dc = &g_dma->channel[ch];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"ch\":%d,\"bAdr\":\"0x%02x\",\"aAdr\":\"0x%04x\",\"aBank\":\"0x%02x\","
            "\"size\":%d,\"mode\":%d,"
            "\"dmaActive\":%s,\"hdmaActive\":%s,\"fixed\":%s,"
            "\"decrement\":%s,\"indirect\":%s,\"fromB\":%s}",
            ch ? "," : "", ch, dc->bAdr, dc->aAdr, dc->aBank,
            dc->size, dc->mode,
            dc->dmaActive ? "true" : "false", dc->hdmaActive ? "true" : "false",
            dc->fixed ? "true" : "false", dc->decrement ? "true" : "false",
            dc->indirect ? "true" : "false", dc->fromB ? "true" : "false");
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

    // DMA channels
    pos = snprintf(buf, sizeof(buf), "\"dma\":[");
    for (int ch = 0; ch < 8; ch++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"bAdr\":\"0x%02x\",\"aBank\":\"0x%02x\",\"mode\":%d,\"flags\":\"0x%02x\","
            "\"aAdr\":\"0x%04x\",\"size\":%d}",
            ch ? "," : "", r->dma[ch].bAdr, r->dma[ch].aBank, r->dma[ch].mode,
            r->dma[ch].flags, r->dma[ch].aAdr, r->dma[ch].size);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],");
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

typedef struct { const char *name; void (*handler)(const char *args); } CmdEntry;
static const CmdEntry s_commands[] = {
    {"ping",          cmd_ping},
    {"frame",         cmd_frame},
    {"read_ram",      cmd_read_ram},
    {"dump_ram",      cmd_dump_ram},
    {"trace_dispatch", cmd_trace_dispatch},
    {"get_dispatch_trace", cmd_get_dispatch_trace},
    {"call_stack",    cmd_call_stack},
    {"watch",         cmd_watch},
    {"unwatch",       cmd_unwatch},
    {"pause",         cmd_pause},
    {"continue",      cmd_continue},
    {"step",          cmd_step},
    {"run_to_frame",  cmd_run_to_frame},
    {"loadstate",     cmd_loadstate},
    {"trace_addr",    cmd_trace_addr},
    {"get_trace",     cmd_get_trace},
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
    {"get_apu_state", cmd_get_apu_state},
    {"dump_apu_ram",  cmd_dump_apu_ram},
    {"screenshot",     cmd_screenshot},
    {"get_frame_extended", cmd_get_frame_extended},
    {"get_frame_range_extended", cmd_get_frame_range_extended},
    {"map16_trace_on",  cmd_map16_trace_on},
    {"map16_trace_off", cmd_map16_trace_off},
    {"get_map16_trace", cmd_get_map16_trace},
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

void debug_server_set_frame_counter(int *counter) {
    (void)counter; // Deprecated — snes_frame_counter used directly
}

void debug_server_set_snapshots(void *mine, void *theirs, void *before) {
    // Store for verify_diff — will implement later
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
