#include "common_cpu_infra.h"
#include "framedump.h"
#include "types.h"
#include "common_rtl.h"
#include "recomp_hw.h"
#include "snes/cpu.h"
#include "snes/snes.h"
#include "util.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "cpu_state.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Snes *g_snes;
Cpu *g_snes_cpu;

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
  g_snes_cpu->e = false;
  g_snes_cpu->sp = 0x01FF;
  g_snes_cpu->dp = 0;
  g_snes_cpu->mf = false;
  g_snes_cpu->xf = false;
  g_snes_cpu->d = false;
  g_snes_cpu->i = true;
}

// Resolve a 16-bit-indirect-through-DP pointer using the current
// data bank register. See comment in common_rtl.h for why this
// matters for `(dp)`, `(dp),Y`, `(dp,X)` addressing modes.
uint8_t *IndirPtrDB(uint8 dp_addr, uint16 offs) {
  LongPtr p = MAKE_LONG((uint16)g_ram[dp_addr] | ((uint16)g_ram[dp_addr + 1] << 8),
                        g_snes_cpu->db);
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

/* Per-frame 65816 stack-entry level (cpu->S at function entry), parallel
 * to g_recomp_stack and indexed by the same g_recomp_stack_top. The
 * function prologue records _entry_s here; pops are implicit (top--).
 * Used by cpu_resolve_ancestor_skip() to turn a return-to-ancestor RTS
 * (manual PLA/PLX/PLB rebalance to an ancestor's entry level, then RTS)
 * into a SKIP_N non-local return through the existing call-site
 * decrement contract. See ISSUES.md "shared-tail multi-level non-local
 * return" (the fish-explosion OAM wipe). */
uint16_t g_cpu_entry_s[RECOMP_STACK_DEPTH];
static uint8_t g_tailcall_context_valid;
static uint16_t g_tailcall_entry_s;
static uint8_t g_tailcall_hrv;

void cpu_tailcall_inherit_return_context(uint16_t entry_s, uint8_t hrv) {
  g_tailcall_entry_s = entry_s;
  g_tailcall_hrv = hrv;
  g_tailcall_context_valid = 1;
}

int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv) {
  if (!g_tailcall_context_valid) return 0;
  if (entry_s) *entry_s = g_tailcall_entry_s;
  if (hrv) *hrv = g_tailcall_hrv;
  g_tailcall_context_valid = 0;
  return 1;
}

void cpu_tailcall_context_save(CpuTailcallContextSave *out) {
  if (!out) return;
  out->valid = g_tailcall_context_valid;
  out->entry_s = g_tailcall_entry_s;
  out->hrv = g_tailcall_hrv;
}

void cpu_tailcall_context_restore(const CpuTailcallContextSave *in) {
  if (!in) return;
  g_tailcall_context_valid = in->valid;
  g_tailcall_entry_s = in->entry_s;
  g_tailcall_hrv = in->hrv;
}

/* ── Unresolved-site balanced abandon + ALWAYS-ON hit accounting ──────
 * Correctness layer (compiled in every configuration, unlike the
 * cpu_trace_* slot table which no-ops at SNESRECOMP_TRACE=0). Every
 * abandon is a real worklist item — a control transfer the recompiler
 * could not resolve whose handler's side effects were skipped — so the
 * hit table is the primary signal for WHICH dispatch sites to
 * authorize next. Bounded per-site table; first hit per site emits one
 * stderr line; the post-mortem report dumps the whole table. */
#define UNRESOLVED_ABANDON_MAX 256
typedef struct UnresolvedAbandonSite {
  uint32_t site_pc24;
  uint64_t hits;
  int32_t first_frame;
  int32_t last_frame;
  uint8_t last_hrv;
} UnresolvedAbandonSite;
static UnresolvedAbandonSite g_unresolved_abandons[UNRESOLVED_ABANDON_MAX];
static int g_unresolved_abandon_count;
static uint64_t g_unresolved_abandon_total;
static uint64_t g_unresolved_abandon_overflow;

RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32_t site_pc24,
                                             uint16_t entry_s, uint8_t hrv) {
  extern int snes_frame_counter;
  /* hrv carries the paired caller's pushed frame SIZE (0/2/3) under the
   * 2026-06-09 host_return_valid encoding. A literal 1 means a setter
   * missed the encoding upgrade — warn once, loudly, and treat it as
   * size-unknown (restore the entry baseline only; the 2-3 byte frame
   * leak it leaves is the pre-fix behavior, strictly no worse). */
  static int warned_legacy_hrv;
  if (hrv == 1) {
    if (!warned_legacy_hrv) {
      warned_legacy_hrv = 1;
      fprintf(stderr,
              "[abandon_balanced] LEGACY hrv==1 seen at $%06X (S=$%04X "
              "entry_s=$%04X) — a host_return_valid setter still writes "
              "boolean 1; frame size unknown, restoring entry baseline only\n",
              (unsigned)site_pc24, cpu->S, entry_s);
    }
    hrv = 0;
  }
  g_unresolved_abandon_total++;
  int i;
  for (i = 0; i < g_unresolved_abandon_count; i++) {
    if (g_unresolved_abandons[i].site_pc24 == site_pc24) break;
  }
  if (i == g_unresolved_abandon_count) {
    if (i < UNRESOLVED_ABANDON_MAX) {
      g_unresolved_abandon_count++;
      g_unresolved_abandons[i].site_pc24 = site_pc24;
      g_unresolved_abandons[i].hits = 0;
      g_unresolved_abandons[i].first_frame = snes_frame_counter;
      fprintf(stderr,
              "[unresolved-abandon] first hit site=$%06X frame=%d "
              "entry_s=$%04X hrv=%u (handler side effects SKIPPED — "
              "authorize this dispatch)\n",
              (unsigned)site_pc24, snes_frame_counter, entry_s, hrv);
    } else {
      g_unresolved_abandon_overflow++;
      i = -1;
    }
  }
  if (i >= 0) {
    g_unresolved_abandons[i].hits++;
    g_unresolved_abandons[i].last_frame = snes_frame_counter;
    g_unresolved_abandons[i].last_hrv = hrv;
  }
  cpu->S = (uint16_t)(entry_s + hrv);
  return RECOMP_RETURN_NORMAL;
}

void CpuUnresolvedAbandonDumpJson(FILE *f) {
  fprintf(f, "  \"unresolved_abandons\": {\n"
             "    \"total_hits\": %llu,\n"
             "    \"distinct_sites\": %d,\n"
             "    \"overflowed_hits\": %llu,\n"
             "    \"sites\": [",
          (unsigned long long)g_unresolved_abandon_total,
          g_unresolved_abandon_count,
          (unsigned long long)g_unresolved_abandon_overflow);
  for (int i = 0; i < g_unresolved_abandon_count; i++) {
    const UnresolvedAbandonSite *s = &g_unresolved_abandons[i];
    fprintf(f, "%s\n      {\"site\": \"0x%06X\", \"hits\": %llu, "
               "\"first_frame\": %d, \"last_frame\": %d, \"last_hrv\": %u}",
            i ? "," : "", (unsigned)s->site_pc24,
            (unsigned long long)s->hits, s->first_frame, s->last_frame,
            s->last_hrv);
  }
  fprintf(f, "\n    ]\n  },\n");
}

int cpu_resolve_ancestor_skip(uint16_t ret_s) {
  /* The current (top-1) frame is the one whose RTS we are resolving; it
   * is NOT a match (its entry_s != ret_s, else the balanced host-return
   * path handled it). Scan STRICT ancestors for the nearest frame whose
   * entry_s == ret_s — that frame should host-return NORMAL to its
   * caller (which resumes at its natural continuation). Return the SKIP
   * count = how many RECOMP_RETURN levels to unwind to reach it; -1 if
   * none (caller falls back to the normal dispatch-miss path, no change
   * in behavior). */
  int top = g_recomp_stack_top;
  if (top < 2 || top > RECOMP_STACK_DEPTH) return -1;
  for (int i = top - 2; i >= 0; i--) {
    if (g_cpu_entry_s[i] == ret_s) return (top - 1) - i;
  }
  return -1;
}

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
  // Boundary auditor (always-on; no-op when SNESRECOMP_TRACE=0).
  // Recorded AFTER the stack push so stack_depth reflects post-push state.
  boundary_audit_record_entry(name);
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

/* ── Always-on stack-balance auditor ──────────────────────────────────────
 * JSL/JSR/RTS/RTL are 0-delta in the codegen (their hardware frame is NOT
 * pushed onto cpu->S — only explicit push/pull move it). So a correctly
 * recompiled function returns with cpu->S == its entry S. A persistent net
 * delta means an unbalanced push/pull on the taken exit path (e.g. a
 * PHP/PHA/PHX/PHY prologue whose matching pulls are skipped by a mis-routed
 * early exit). We accumulate net delta per function name (pointer-keyed —
 * the generated code passes interned string literals) so a per-frame leaker
 * stands out by sheer magnitude. Always on; dumped by the watchdog/crash
 * path and the post-mortem report. */
#define STACKBAL_MAX 2048
typedef struct {
  const char *name;
  long long   total_delta;  /* sum of (exit_s - entry_s) across all returns */
  long        calls;
  long        nonzero;      /* # returns with a nonzero delta */
  int         last_delta;
} StackBalEntry;
static StackBalEntry g_stackbal[STACKBAL_MAX];

static StackBalEntry *stackbal_find(const char *name) {
  unsigned h = (unsigned)(((uintptr_t)name >> 4) & (STACKBAL_MAX - 1));
  for (int i = 0; i < STACKBAL_MAX; i++) {
    unsigned idx = (h + i) & (STACKBAL_MAX - 1);
    if (g_stackbal[idx].name == name) return &g_stackbal[idx];
    if (g_stackbal[idx].name == NULL) { g_stackbal[idx].name = name; return &g_stackbal[idx]; }
  }
  return NULL; /* table full — drop */
}

/* Comparator helper: collect non-zero-delta entries into `out`, return count. */
static int stackbal_collect(StackBalEntry **out, int cap) {
  int n = 0;
  for (int i = 0; i < STACKBAL_MAX && n < cap; i++)
    if (g_stackbal[i].name && g_stackbal[i].total_delta != 0)
      out[n++] = &g_stackbal[i];
  /* simple selection sort by |total_delta| desc (n is small) */
  for (int a = 0; a < n; a++) {
    int best = a;
    for (int b = a + 1; b < n; b++)
      if (llabs(out[b]->total_delta) > llabs(out[best]->total_delta)) best = b;
    StackBalEntry *t = out[a]; out[a] = out[best]; out[best] = t;
  }
  return n;
}

void RecompStackBalDumpStderr(int topn) {
  StackBalEntry *top[64];
  if (topn > 64) topn = 64;
  int n = stackbal_collect(top, topn);
  fprintf(stderr, "=== stack-balance auditor: top %d net-imbalanced funcs ===\n", n);
  for (int i = 0; i < n; i++)
    fprintf(stderr, "  %+lld bytes net over %ld calls (%ld nonzero, last %+d): %s\n",
            top[i]->total_delta, top[i]->calls, top[i]->nonzero,
            top[i]->last_delta, top[i]->name ? top[i]->name : "?");
  fflush(stderr);
}

void RecompStackBalDumpJson(FILE *f) {
  StackBalEntry *top[64];
  int n = stackbal_collect(top, 40);
  fprintf(f, "  \"stack_balance\": [");
  for (int i = 0; i < n; i++)
    fprintf(f, "%s{\"name\":\"%s\",\"total_delta\":%lld,\"calls\":%ld,"
               "\"nonzero\":%ld,\"last_delta\":%d}",
            (i ? "," : ""), top[i]->name ? top[i]->name : "?",
            top[i]->total_delta, top[i]->calls, top[i]->nonzero, top[i]->last_delta);
  fprintf(f, "],\n");
}

void RecompStackPop(void) {
  // Record exit BEFORE the pop so stack_depth reflects pre-pop state and
  // the function name is still the topmost entry. Defensive against
  // empty stack: the auditor must NOT consume an entry_seq it didn't push.
  if (g_recomp_stack_top > 0) {
    const char *fn = g_recomp_stack[g_recomp_stack_top - 1];
    int delta = (int)(int16_t)(g_cpu.S - g_cpu_entry_s[g_recomp_stack_top - 1]);
    StackBalEntry *e = stackbal_find(fn);
    if (e) {
      e->calls++;
      if (delta) { e->total_delta += delta; e->nonzero++; e->last_delta = delta; }
    }
    boundary_audit_record_exit(g_recomp_stack[g_recomp_stack_top - 1]);
    g_recomp_stack_top--;
  }
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
  g_tailcall_context_valid = 0;
}

// Called at loop headers in generated code — detect infinite loops
void WatchdogCheck(void) {
  /* Boot-spin observability. During boot (frame 0) the normal frame
   * watchdog is disabled below (the SPC IPL upload is legitimately slow,
   * real-time paced by the audio thread). But a genuine boot spin
   * (I_RESET never reaching the first WaitForNMI yield) otherwise hangs
   * SILENTLY: the host's SwitchToFiber never returns, the window goes
   * black + "Not Responding", and frame_counter stays 0 forever so the
   * frame watchdog never arms. Sample the live recomp call stack at
   * intervals during boot so (a) a true spin's location is named and
   * (b) a slow-but-progressing upload (stack CHANGES between samples) is
   * distinguishable from a real spin (stack IDENTICAL). Pure sampling:
   * no longjmp (the boot fiber has no valid g_watchdog_jmp setjmp frame)
   * and no abort, so a healthy slow boot is never disturbed — once the
   * first NMI bumps frame_counter past 0 this whole branch goes dormant.
   * Tunable via SNESRECOMP_BOOT_WATCHDOG_SECS (default: first sample at
   * 8 s, then every 3 s). Shared across every game's bring-up. */
  if (snes_frame_counter == 0) {
    static long    boot_calls = 0;
    static clock_t boot_first = 0, boot_last_print = 0;
    static double  boot_first_secs = -1.0;
    if (boot_first == 0) boot_first = clock();
    if (++boot_calls % 2000 != 0) return;   /* throttle clock() reads */
    if (boot_first_secs < 0.0) {
      const char *e = getenv("SNESRECOMP_BOOT_WATCHDOG_SECS");
      boot_first_secs = e ? atof(e) : 8.0;
    }
    clock_t now = clock();
    double since_start = (double)(now - boot_first) / CLOCKS_PER_SEC;
    double since_print = (double)(now - boot_last_print) / CLOCKS_PER_SEC;
    if (since_start > boot_first_secs && since_print > 3.0) {
      boot_last_print = now;
      fprintf(stderr,
        "[boot-watchdog] %.1fs in boot (frame 0); recomp depth=%d, "
        "top=%s\n", since_start, g_recomp_stack_top,
        g_recomp_stack_top > 0 ? g_recomp_stack[g_recomp_stack_top - 1]
                               : (g_last_recomp_func ? g_last_recomp_func
                                                     : "(none)"));
      for (int i = g_recomp_stack_top - 1, n = 0; i >= 0 && n < 16; i--, n++)
        fprintf(stderr, "    [%d] %s\n", n, g_recomp_stack[i]);
      fflush(stderr);
    }
    return;
  }
  if (!g_watchdog_enabled) return;
  // Only check clock() every 10000 iterations to avoid overhead
  if (++g_watchdog_counter < 10000) return;
  g_watchdog_counter = 0;
  double elapsed = (double)(clock() - g_frame_start_clock) / CLOCKS_PER_SEC;
  /* Boot has no watchdog. I_RESET runs once and uploads the SPC
   * engine + samples through the IPL handshake, which is real-time
   * paced by the audio thread (the SPC bootROM can only echo bytes
   * at ~1 MHz). For SMW the upload is ~12 KB and naturally takes
   * tens of seconds wall time; that's expected, not a hang. After
   * I_RESET returns the runtime falls into the normal per-frame
   * cadence (I_NMI + Internal) which completes comfortably under 5 s.
   *
   * Detecting "still in boot" via snes_frame_counter == 0 is robust:
   * the recompiled NMI handler increments snes_frame_counter, and
   * the very first NMI only fires after I_RESET returns and frame 1
   * starts. */
  if (snes_frame_counter == 0) return;
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
    RecompStackBalDumpStderr(15);
    fflush(stderr);
    g_watchdog_enabled = 0;
    g_watchdog_tripped = 1;
    { extern int snes_frame_counter;
      debug_server_profile_latch(snes_frame_counter); }
    longjmp(g_watchdog_jmp, 1);
  }
}

Snes *SnesInit(const uint8 *data, int data_size) {
  g_snes = snes_init(g_ram);
  g_snes_cpu = g_snes->cpu;
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

