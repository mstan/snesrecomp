#include "common_cpu_infra.h"
#include "framedump.h"
#include "types.h"
#include "common_rtl.h"
#include "recomp_state.h"
#include "recomp_hw.h"
#include "snes/cpu.h"
#include "snes/snes.h"
#include "tracing.h"
#include "util.h"
#include <time.h>

Snes *g_snes;
Cpu *g_cpu;

bool g_calling_asm_from_c;
int g_calling_asm_from_c_ret;
bool g_fail;
// HLE SPC. Full HLE→real switch is a multi-day framework effort — the
// recompiler emits the SPC upload routine with multiple M-flag width
// bugs (polling 16-bit A against an 8-bit port echo; missing writes
// inside the per-byte loop), unrelated to the V-flag issue already
// fixed. Real-APU mode works for the $AABB handshake + the single
// $CC echo, then hangs inside the per-byte data loop. Leaving HLE
// as the default until the recompiler's M-flag tracking is broader.
bool g_use_my_apu_code = true;
extern bool g_other_image;
const RtlGameInfo *g_rtl_game_info;

static uint32 hookmode, hookcnt, hookadr;
static uint32 hooked_func_pc;
static uint8 hook_orgbyte[1024];
static uint8 hook_fixbug_orgbyte[1024];
static uint8 kPatchedCarrysOrg[1024];

void MakeSnapshot(Snapshot *s);
void RestoreSnapshot(Snapshot *s);

uint8_t *SnesRomPtr(uint32 v) {
  return (uint8 *)RomPtr(v);
}

bool ProcessHook(uint32 v) {
  uint8_t *rombyte = SnesRomPtr(v);
  switch (hookmode) {
  case 0: // remove hooks
    *rombyte = hook_orgbyte[hookcnt++];
    return false;
  case 1: // install hooks
    hook_orgbyte[hookcnt++] = *rombyte;
    *rombyte = 0;
    return false;
  case 2:  // run hook
    if (v == hookadr) {
      hookmode = 3;
      return true;
    }
    return false;
  }
  return false;
}

bool FixBugHook(uint32 addr) {
  switch (hookmode) {
  case 1: { // install hooks
    uint8_t *rombyte = SnesRomPtr(addr);
    hook_fixbug_orgbyte[hookcnt++] = *rombyte;
    *rombyte = 0;
    return false;
  }
  case 2:  // run hook
    if (addr == hookadr) {
      hookmode = 3;
      return true;
    }
    hookcnt++;
    return false;
  }
  return false;
}

uint32 PatchBugs(uint32 mode, uint32 addr) {
  hookmode = mode, hookadr = addr, hookcnt = 0;
  return g_rtl_game_info->patch_bugs();
}

int RunPatchBugHook(uint32 addr) {
  uint32 new_pc = PatchBugs(2, addr);
  if (hookmode == 3) {
    if (new_pc == 0) {
      return hook_fixbug_orgbyte[hookcnt];
    } else {
      g_cpu->k = new_pc >> 16;
      g_cpu->pc = (new_pc & 0xffff) + 1;
      return *SnesRomPtr(new_pc);
    }
  }
  return -1;
}

int CpuOpcodeHook(uint32 addr) {
  for (size_t i = 0; i != g_rtl_game_info->patch_carrys_count; i++) {
    if (addr == g_rtl_game_info->patch_carrys[i]) {
      return kPatchedCarrysOrg[i];
    }
  }
  {
    int i = RunPatchBugHook(addr);
    if (i >= 0) return i;
  }
  printf("Bad hook at 0x%x!\n", addr);
  assert(0);
  return 0;
}

bool HookedFunctionRts(int is_long) {
  if (g_calling_asm_from_c) {
    g_calling_asm_from_c_ret = is_long;
    g_calling_asm_from_c = false;
    return false;
  }
  assert(0);
  return false;
}

// Debug: recomp function call stack for watchdog diagnostics.
const char *g_last_recomp_func = "(none)";
#define RECOMP_STACK_DEPTH 16
const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int g_recomp_stack_top = 0;

extern void debug_server_profile_push(const char *name);
FILE *g_boot_trace_file = NULL;
int g_boot_trace_frames = 0;
void RecompStackPush(const char *name) {
  if (g_recomp_stack_top < RECOMP_STACK_DEPTH)
    g_recomp_stack[g_recomp_stack_top++] = name;
  g_last_recomp_func = name;
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
int g_indirptr_count;

void WatchdogFrameStart(void) {
  g_frame_start_clock = clock();
  g_watchdog_enabled = 1;
  g_watchdog_tripped = 0;
  g_watchdog_counter = 0;
  g_indirptr_count = 0;
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
      "Game mode: %d | IndirPtr calls: %d | WatchdogCheck calls: %d\n"
      "Call stack (most recent first):\n",
      snes_frame_counter, elapsed, g_ram[0x100], g_indirptr_count, g_watchdog_counter * 10000);
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

void MakeSnapshot(Snapshot *s) {
  Cpu *c = g_cpu;
  s->a = c->a, s->x = c->x, s->y = c->y;
  s->sp = c->sp, s->dp = c->dp, s->db = c->db;
  s->pc = c->pc, s->k = c->k;
  s->flags = cpu_getFlags(c);
  s->vTimer = g_snes->vTimer;
  memcpy(s->ram, g_snes->ram, 0x20000);
  memcpy(s->sram, g_snes->cart->ram, g_snes->cart->ramSize);
  memcpy(s->vram, g_ppu->vram, sizeof(uint16) * 0x8000);
  memcpy(s->oam, g_ppu->oam, sizeof(uint16) * 0x120);
  memcpy(s->cgram, g_ppu->cgram, sizeof(uint16) * 0x100);
}

void RestoreSnapshot(Snapshot *s) {
  Cpu *c = g_cpu;
  c->a = s->a, c->x = s->x, c->y = s->y;
  c->sp = s->sp, c->dp = s->dp, c->db = s->db;
  c->pc = s->pc, c->k = s->k;
  g_snes->vTimer = s->vTimer;
  cpu_setFlags(c, s->flags);
  memcpy(g_snes->ram, s->ram, 0x20000);
  memcpy(g_snes->cart->ram, s->sram, g_snes->cart->ramSize);
  memcpy(g_ppu->vram, s->vram, sizeof(uint16) * 0x8000);
  memcpy(g_ppu->oam, s->oam, sizeof(uint16) * 0x120);
  memcpy(g_ppu->cgram, s->cgram, sizeof(uint16) * 0x100);
}

static void FixupCarry(uint32 addr) {
  *SnesRomPtr(addr) = 0;
}
  
Snes *SnesInit(const uint8 *data, int data_size) {
  g_my_ppu = ppu_init();
  ppu_reset(g_my_ppu);

  g_snes = snes_init(g_ram);
  g_cpu = g_snes->cpu;
  g_dma = g_snes->dma;
  g_use_my_apu_code = true;

  if (data_size != 0) {
    bool loaded = snes_loadRom(g_snes, data, data_size);
    if (!loaded) {
      return NULL;
    }
    g_rom = g_snes->cart->rom;

    g_rtl_game_info = &kSmwGameInfo;

    for (size_t i = 0; i != g_rtl_game_info->patch_carrys_count; i++) {
      uint8 t = *SnesRomPtr(g_rtl_game_info->patch_carrys[i]);
      if (t) {
        kPatchedCarrysOrg[i] = t;
        FixupCarry(g_rtl_game_info->patch_carrys[i]);
      } else {
        printf("0x%x double patched!\n", g_rtl_game_info->patch_carrys[i]);
      }
    }
    g_rtl_game_info->initialize();
    snes_reset(g_snes, true); // reset after loading
    PatchBugs(1, 0);
    // The real ROM's reset vector ($00:8000) sets up CPU state:
    //   $801B: CLC; XCE    → native mode (e=0)
    //   $801D: REP #$38    → 16-bit A/X/Y, clear decimal
    //   $801F: LDA #$0000; TCD  → DP=0
    //   $8023: LDA #$01FF; TCS  → SP=$01FF
    // The recomp path never executes these opcodes, so apply them here.
    g_cpu->e = false;
    g_cpu->sp = 0x01FF;
    g_cpu->dp = 0;
    g_cpu->mf = false;
    g_cpu->xf = false;
    g_cpu->d = false;
    g_cpu->i = true;  // SEI at $8000
    cpu_setFlags(g_cpu, cpu_getFlags(g_cpu));
    recomp_hw_init();
    recomp_sync_from_emu();
  } else {
    g_snes->cart->ramSize = 2048;
    g_snes->cart->ram = calloc(1, 2048);
    g_rtl_game_info = &kSmwGameInfo;
    g_rtl_game_info->initialize();
    ppu_reset(g_snes->ppu);
    dma_reset(g_snes->dma);
  }

  g_sram = g_snes->cart->ram;
  g_sram_size = g_snes->cart->ramSize;
  game_id = g_rtl_game_info->game_id;

  // Copy the emulator's PPU state to g_my_ppu so the recomp path
  // has a properly initialized PPU.
  if (g_my_ppu && g_snes->ppu)
    memcpy(g_my_ppu, g_snes->ppu, sizeof(*g_my_ppu));

  return g_snes;
}

void RtlRunFrameCompare() {
  WatchdogFrameStart();
  g_ppu = g_my_ppu;
  g_snes->runningWhichVersion = 2;
  recomp_sync_from_emu();
  g_rtl_game_info->run_frame();
  g_snes->runningWhichVersion = 0;
  if (g_framedump_callback)
    g_framedump_callback(snes_frame_counter, g_ram, NULL);
  {
    extern void debug_server_record_frame(int);
    debug_server_record_frame(snes_frame_counter);
  }
}
