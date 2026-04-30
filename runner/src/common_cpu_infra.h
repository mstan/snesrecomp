#pragma once

#include "types.h"

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
#include <setjmp.h>
extern jmp_buf g_watchdog_jmp;
extern int g_watchdog_tripped;

typedef struct RtlGameInfo {
  const char *title;
  CpuInfraInitializeFunc *initialize;
  RunOneFrameOfGameFunc *run_frame;
  RunOneFrameOfGameFunc *draw_ppu_frame;
  // Filename prefix used by RtlSaveLoad, e.g. "save" produces
  // "saves/save%d.sav". If NULL, framework uses "%s_save" with title.
  const char *save_name_prefix;
} RtlGameInfo;

extern const RtlGameInfo *g_rtl_game_info;

// Called by the game-layer before SnesInit so the framework knows
// which game it's running. Framework itself names no specific game.
void RtlRegisterGame(const RtlGameInfo *info);