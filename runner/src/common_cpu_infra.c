#include "common_cpu_infra.h"
#include "framedump.h"
#include "types.h"
#include "common_rtl.h"
#include "recomp_hw.h"
#include "snes/cpu.h"
#include "snes/snes.h"
#include "util.h"
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
#define RECOMP_STACK_DEPTH 16
const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int g_recomp_stack_top = 0;

extern void debug_server_profile_push(const char *name);
void RecompStackPush(const char *name) {
  if (g_recomp_stack_top < RECOMP_STACK_DEPTH)
    g_recomp_stack[g_recomp_stack_top++] = name;
  g_last_recomp_func = name;
  debug_server_profile_push(name);
}

void RecompStackDump(void) {
  fprintf(stderr, "Recomp call stack (%d deep):\n", g_recomp_stack_top);
  for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 16; i--)
    fprintf(stderr, "  [%d] %s\n", g_recomp_stack_top - 1 - i, g_recomp_stack[i]);
}

void RecompStackPop(void) {
  if (g_recomp_stack_top > 0)
    g_recomp_stack_top--;
  g_last_recomp_func = g_recomp_stack_top > 0 ? g_recomp_stack[g_recomp_stack_top - 1] : "(none)";
}

// Frame watchdog: detect infinite loops in generated code.
// Set before calling run_frame, checked by generated code periodically.
#include <setjmp.h>
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

