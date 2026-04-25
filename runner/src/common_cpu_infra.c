#include "common_cpu_infra.h"
#include "framedump.h"
#include "types.h"
#include "common_rtl.h"
#include "recomp_hw.h"
#include "snes/cpu.h"
#include "snes/snes.h"
#include "util.h"
#include <setjmp.h>
#include <string.h>
#include <time.h>

Snes *g_snes;
Cpu *g_cpu;

bool g_fail;
const RtlGameInfo *g_rtl_game_info;

void RtlRegisterGame(const RtlGameInfo *info) {
  g_rtl_game_info = info;
}

uint8_t *SnesRomPtr(uint32 v) {
  return (uint8 *)RomPtr(v);
}

// Apply the native-mode CPU state the real ROM's reset vector would
// have established. See header comment.
void SnesEnterNativeMode(void) {
  g_cpu->e = false;
  g_cpu->sp = 0x01FF;
  g_cpu->dp = 0;
  g_cpu->mf = false;
  g_cpu->xf = false;
  g_cpu->d = false;
  g_cpu->i = true;
}

// Resolve a 16-bit-indirect-through-DP pointer using the current
// data bank register. See comment in common_rtl.h for why this
// matters for `(dp)`, `(dp),Y`, `(dp,X)` addressing modes.
uint8_t *IndirPtrDB(uint8 dp_addr, uint16 offs) {
  LongPtr p = MAKE_LONG((uint16)g_ram[dp_addr] | ((uint16)g_ram[dp_addr + 1] << 8),
                        g_cpu->db);
  return IndirPtr(p, offs);
}

// Debug: recomp function call stack for watchdog diagnostics.
const char *g_last_recomp_func = "(none)";
// Tier 1.5 call-trace depth cap. Originally 16; bumped to 64 because
// SMW peak call depth is ~10 but Tier 1.5 attribution silently
// degrades past the cap (g_last_recomp_func and parent fields go
// stale). 64 gives 6x headroom for any conceivable call chain at
// negligible memory cost (8 bytes/slot * 48 extra slots = 384 bytes).
#define RECOMP_STACK_DEPTH 64
const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int g_recomp_stack_top = 0;

extern void debug_server_profile_push(const char *name);

// Function-boundary WRAM snapshot history (Phase B koopa-stomp).
// When a TCP client sets g_recomp_snap_on_func to a non-NULL name,
// every RecompStackPush whose name matches captures the LOW 8KB of
// WRAM ($0000-$1FFF — DP + game-state region used by SMW for all
// sprite/level/player state) into a ring buffer of 256 slots.
//
// Ring keeps the most recent 256 calls; older entries get overwritten.
// Each slot has: absolute call index (the count at capture time),
// frame number at capture, and the 8KB WRAM slice. Total: 256 × 8KB
// = 2 MB per side. Fits comfortably; 256 calls ≈ 4 seconds at 60Hz
// and ≈ 256 frames in SMW (one HandlePlayerPhysics call per frame).
//
// Probes use func_snap_get_n <call_idx> to fetch a specific historic
// snapshot and bisect for the first diverging call.
#define RECOMP_SNAP_SLICE_LEN  0x2000  /* $0000-$1FFF */
#define RECOMP_SNAP_RING_LEN   256

const char *g_recomp_snap_on_func = NULL;
int        g_recomp_snap_count    = 0;     /* total calls observed */
int        g_recomp_snap_frame    = -1;    /* most recent capture's frame */
typedef struct {
    int     call_idx;                       /* g_recomp_snap_count value at capture */
    int     frame;
    uint8_t wram_slice[RECOMP_SNAP_SLICE_LEN];
} recomp_snap_entry;
recomp_snap_entry g_recomp_snap_ring[RECOMP_SNAP_RING_LEN];

/* Lookup an entry by absolute call index. Returns NULL if the index
 * is out of the ring's current window. */
const recomp_snap_entry* recomp_snap_lookup(int call_idx) {
    if (call_idx < 1) return NULL;
    int slot = (call_idx - 1) % RECOMP_SNAP_RING_LEN;
    if (g_recomp_snap_ring[slot].call_idx != call_idx) return NULL;
    return &g_recomp_snap_ring[slot];
}

void RecompStackPush(const char *name) {
  if (g_recomp_stack_top < RECOMP_STACK_DEPTH)
    g_recomp_stack[g_recomp_stack_top++] = name;
  g_last_recomp_func = name;
  debug_server_profile_push(name);
  // Function-boundary snapshot: if a client set a target function
  // name, and this push matches it, capture WRAM. Frame execution
  // continues afterward — no longjmp. Compare the snapshot at
  // matching points across recomp + oracle for sub-frame-precise
  // state diff regardless of NMI ordering.
  if (g_recomp_snap_on_func) {
    extern int snes_frame_counter;
    int match;
    if (name == g_recomp_snap_on_func) match = 1;
    else if (strcmp(g_recomp_snap_on_func, name) == 0) {
      g_recomp_snap_on_func = name;  /* cache pointer for fast path */
      match = 1;
    } else {
      match = 0;
    }
    if (match) {
      g_recomp_snap_count++;
      g_recomp_snap_frame = snes_frame_counter;
      int slot = (g_recomp_snap_count - 1) % RECOMP_SNAP_RING_LEN;
      g_recomp_snap_ring[slot].call_idx = g_recomp_snap_count;
      g_recomp_snap_ring[slot].frame    = snes_frame_counter;
      memcpy(g_recomp_snap_ring[slot].wram_slice, g_ram, RECOMP_SNAP_SLICE_LEN);
    }
  }
}

void RecompStackDump(void) {
  fprintf(stderr, "Recomp call stack (%d deep):\n", g_recomp_stack_top);
  for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - RECOMP_STACK_DEPTH; i--)
    fprintf(stderr, "  [%d] %s\n", g_recomp_stack_top - 1 - i, g_recomp_stack[i]);
}

void RecompStackPop(void) {
  if (g_recomp_stack_top > 0)
    g_recomp_stack_top--;
  g_last_recomp_func = g_recomp_stack_top > 0 ? g_recomp_stack[g_recomp_stack_top - 1] : "(none)";
}

// Frame watchdog: detect infinite loops in generated code.
// Set before calling run_frame, checked by generated code periodically.
static clock_t g_frame_start_clock;
static int g_watchdog_enabled;
static int g_watchdog_counter;
jmp_buf g_watchdog_jmp;
int g_watchdog_tripped;

void WatchdogFrameStart(void) {
  g_frame_start_clock = clock();
  g_watchdog_enabled = 1;
  g_watchdog_tripped = 0;
  g_watchdog_counter = 0;
  g_recomp_stack_top = 0;
}

// Called at loop headers in generated code — detect infinite loops
void WatchdogCheck(void) {
  if (!g_watchdog_enabled) return;
  // Only check clock() every 10000 iterations to avoid overhead
  if (++g_watchdog_counter < 10000) return;
  g_watchdog_counter = 0;
  double elapsed = (double)(clock() - g_frame_start_clock) / CLOCKS_PER_SEC;
  if (elapsed > 5.0) {
    fprintf(stderr,
      "\n=== WATCHDOG: Frame %d exceeded %.1fs ===\n"
      "Game mode: %d | WatchdogCheck calls: %d\n"
      "Call stack (most recent first):\n",
      snes_frame_counter, elapsed, g_ram[0x100], g_watchdog_counter * 10000);
    for (int i = g_recomp_stack_top - 1; i >= 0; i--)
      fprintf(stderr, "  [%d] %s\n", g_recomp_stack_top - 1 - i, g_recomp_stack[i]);
    if (g_recomp_stack_top == 0)
      fprintf(stderr, "  (empty — last was %s)\n", g_last_recomp_func);
    fprintf(stderr, "\n");
    fflush(stderr);
    g_watchdog_enabled = 0;
    g_watchdog_tripped = 1;
    { extern void debug_server_profile_latch(int);
      extern int snes_frame_counter;
      debug_server_profile_latch(snes_frame_counter); }
    longjmp(g_watchdog_jmp, 1);
  }
}

Snes *SnesInit(const uint8 *data, int data_size) {
  g_snes = snes_init(g_ram);
  g_cpu = g_snes->cpu;
  g_dma = g_snes->dma;
  g_ppu = g_snes->ppu;

  if (data_size != 0) {
    bool loaded = snes_loadRom(g_snes, data, data_size);
    if (!loaded) {
      return NULL;
    }
    g_rom = g_snes->cart->rom;

    assert(g_rtl_game_info && "RtlRegisterGame must be called before SnesInit");

    if (g_rtl_game_info->initialize)
      g_rtl_game_info->initialize();
    snes_reset(g_snes, true); // reset after loading
    SnesEnterNativeMode();
  } else {
    g_snes->cart->ramSize = 2048;
    g_snes->cart->ram = calloc(1, 2048);
    assert(g_rtl_game_info && "RtlRegisterGame must be called before SnesInit");
    if (g_rtl_game_info->initialize)
      g_rtl_game_info->initialize();
    ppu_reset(g_snes->ppu);
    dma_reset(g_snes->dma);
  }

  g_sram = g_snes->cart->ram;
  g_sram_size = g_snes->cart->ramSize;
  return g_snes;
}

