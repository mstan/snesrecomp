#pragma once

#include "types.h"

#ifdef _MSC_VER
#pragma warning(disable: 4013 4028 4033 4090 4133 4305 4715 4716)
#endif

typedef struct Snes Snes;
typedef struct Cpu Cpu;
typedef struct Snapshot Snapshot;

extern Snes *g_snes;
extern Cpu *g_cpu;
extern bool g_fail;

typedef struct Snes Snes;

Snes *SnesInit(const uint8 *data, int data_size);
bool FixBugHook(uint32 addr);
uint8_t *SnesRomPtr(uint32 v);

typedef uint32 PatchBugsFunc(void);
typedef void CpuInfraInitializeFunc(void);
typedef void RunOneFrameOfGameFunc(void);

void RtlRunFrameCompare(void);
void WatchdogCheck(void);
void WatchdogFrameStart(void);
void RecompStackPush(const char *name);
void RecompStackPop(void);
#include <setjmp.h>
extern jmp_buf g_watchdog_jmp;
extern int g_watchdog_tripped;
void MakeSnapshot(Snapshot *s);
void RestoreSnapshot(Snapshot *s);

typedef struct RtlGameInfo {
  const char *title;
  uint8 game_id;
  const uint32 *patch_carrys;
  size_t patch_carrys_count;
  PatchBugsFunc *patch_bugs;
  CpuInfraInitializeFunc *initialize;
  RunOneFrameOfGameFunc *run_frame;
  RunOneFrameOfGameFunc *draw_ppu_frame;
  // Filename prefix used by RtlSaveLoad for slot < 256 saves, e.g. "save"
  // produces "saves/save%d.sav". If NULL, framework uses "%s_save" with title.
  const char *save_name_prefix;
  // Optional per-frame hook invoked after state recording; receives the
  // resolved input word. Used for game-specific RAM reflection.
  void (*on_frame_inputs)(uint32 inputs);
  // Optional hook invoked when g_did_finish_level_hook trips.
  void (*on_finish_level)(void);
  // Optional override for RtlSaveLoad when slot >= 256 (bug/playback saves).
  // Returns true if the game consumed the call; false to fall through.
  bool (*special_save_load)(int cmd, int slot);
} RtlGameInfo;

typedef struct Snapshot {
  uint16 a, x, y, sp, dp, pc;
  uint8 k, db, flags;

  uint16_t vTimer;

  uint8 ram[0x20000];
  uint16 vram[0x8000];
  uint8 sram[0x2000];

  uint16 oam[0x120];
  uint16 cgram[0x100];
} Snapshot;

extern const RtlGameInfo *g_rtl_game_info;

// Called by the game-layer before SnesInit so the framework knows
// which game it's running. Framework itself names no specific game.
void RtlRegisterGame(const RtlGameInfo *info);