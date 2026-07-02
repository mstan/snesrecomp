#pragma once

#include "types.h"
#include <stdio.h>

#ifdef _MSC_VER
#pragma warning(disable: 4013 4028 4033 4090 4133 4305 4715 4716)
#endif

typedef struct Snes Snes;
typedef struct Cpu Cpu;

extern Snes *g_snes;
extern Cpu *g_snes_cpu;
extern bool g_fail;

Snes *SnesInit(const uint8 *data, int data_size);
uint8_t *SnesRomPtr(uint32 v);

// Apply the native-mode CPU state the real ROM's reset vector would
// have established (CLC;XCE / REP #$38 / TCD / TCS / SEI). The recomp
// path never executes those opcodes, so RtlReset and SnesInit invoke
// this after snes_reset to pick up where the ROM would be at $8028.
void SnesEnterNativeMode(void);

typedef void CpuInfraInitializeFunc(void);
typedef void RunOneFrameOfGameFunc(void);

void WatchdogCheck(void);
void WatchdogFrameStart(void);
void RecompStackPush(const char *name);
void RecompStackPop(void);
/* Always-on stack-balance auditor (see common_cpu_infra.c): reports
 * functions that return with cpu->S != their entry S (unbalanced push/pull). */
void RecompStackBalDumpStderr(int topn);
void RecompStackBalDumpJson(FILE *f);
/* Always-on unresolved-abandon hit table (see cpu_unresolved_abandon_balanced
 * in cpu_state.h): one JSON object per distinct unresolved dispatch/stub/goto
 * site that actually fired this run — the authorization worklist, ordered by
 * first hit. Emits a trailing comma like the other dump_*_json sections. */
void CpuUnresolvedAbandonDumpJson(FILE *f);
/* Always-on dispatch ring (see cpu_state.c): the last DISPATCH_LOG_CAP runtime
 * indirect dispatches (cpu_dispatch_pc_from / cpu_dispatch_call_pc). `found:0`
 * entries ran on the interpreter tier — the AOT-promotion worklist. Trailing
 * comma like the other dump_*_json sections. */
void CpuDispatchLogDumpJson(FILE *f);
/* Per-frame 65816 entry-S tracking for return-to-ancestor RTS resolution
 * (see common_cpu_infra.c). The emitted function prologue records
 * _entry_s into g_cpu_entry_s[g_recomp_stack_top-1]. */
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
int cpu_resolve_ancestor_skip(uint16_t ret_s);
typedef struct CpuTailcallContextSave {
  uint8_t valid;
  uint16_t entry_s;
  uint8_t hrv;
} CpuTailcallContextSave;
void cpu_tailcall_inherit_return_context(uint16_t entry_s, uint8_t hrv);
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv);
void cpu_tailcall_context_save(CpuTailcallContextSave *out);
void cpu_tailcall_context_restore(const CpuTailcallContextSave *in);
#include <setjmp.h>
extern jmp_buf g_watchdog_jmp;
extern int g_watchdog_tripped;

struct SaveLoadInfo;

typedef struct RtlGameInfo {
  const char *title;
  CpuInfraInitializeFunc *initialize;
  RunOneFrameOfGameFunc *run_frame;
  RunOneFrameOfGameFunc *draw_ppu_frame;
  // Filename prefix used by RtlSaveLoad, e.g. "save" produces
  // "saves/save%d.sav". If NULL, framework uses "%s_save" with title.
  const char *save_name_prefix;
  /* Optional save-state extension hooks — all NULL-safe (SMW/Zelda leave
   * them unset). state_save_extra streams a game-specific chunk appended
   * after the snes_saveload blob (format v5+); state_load_extra reads it
   * back (only called when the file carries one). on_state_loaded fires
   * after EVERY successful load, with the file's format version, so the
   * game can reconcile host-side execution state that the guest snapshot
   * cannot capture (e.g. MMX tears down and rebuilds its task fibers). */
  void (*state_save_extra)(struct SaveLoadInfo *sli);
  void (*state_load_extra)(struct SaveLoadInfo *sli, uint32_t version);
  void (*on_state_loaded)(uint32_t version);
} RtlGameInfo;

extern const RtlGameInfo *g_rtl_game_info;

// Called by the game-layer before SnesInit so the framework knows
// which game it's running. Framework itself names no specific game.
void RtlRegisterGame(const RtlGameInfo *info);
