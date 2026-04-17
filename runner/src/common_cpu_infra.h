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
typedef void FixSnapshotForCompareFunc(Snapshot *b, Snapshot *a);

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
  RunOneFrameOfGameFunc *run_frame_emulated;
  RunOneFrameOfGameFunc *draw_ppu_frame;
  FixSnapshotForCompareFunc *fix_snapshot_for_compare;
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

extern const RtlGameInfo kSmwGameInfo;
extern const RtlGameInfo *g_rtl_game_info;