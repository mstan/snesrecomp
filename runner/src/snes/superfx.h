#ifndef SNESRECOMP_SUPERFX_H
#define SNESRECOMP_SUPERFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SuperFxReg {
  uint16_t data;
  bool modified;
} SuperFxReg;

typedef struct SuperFxPixelCache {
  uint16_t offset;
  uint8_t bitpend;
  uint8_t data[8];
} SuperFxPixelCache;

typedef struct SuperFxTraceEntry {
  uint64_t sequence;
  uint16_t r15;
  uint16_t sfr;
  uint8_t pbr;
  uint8_t opcode;
} SuperFxTraceEntry;

/* Host-side presentation enhancements. The default is always the faithful
 * hardware execution path; enhanced modes must be selected explicitly by a
 * title and never change the authoritative GSU registers, RAM, or native
 * framebuffer. */
typedef enum SuperFxEnhancementMode {
  kSuperFxEnhancement_None = 0,
  kSuperFxEnhancement_WidescreenLinearProjection = 1,
} SuperFxEnhancementMode;

/* Architectural state for the Nintendo GSU/Super FX coprocessor.  This is a
 * correctness-oriented LLE core: callers expose the real register and memory
 * buses and advance it from the shared SNES master-clock timeline. */
typedef struct SuperFx {
  uint8_t *rom;
  uint32_t rom_size, rom_mask;
  uint8_t *ram;
  uint32_t ram_size, ram_mask;

  SuperFxReg r[16];
  uint16_t sfr;
  uint8_t pbr, rombr, rambr;
  uint16_t cbr;
  uint8_t scbr, scmr, colr, por, bramr, vcr, cfgr, clsr;
  uint8_t pipeline;
  uint16_t ramaddr;
  uint8_t sreg, dreg;

  uint32_t romcl;
  uint8_t romdr;
  uint32_t ramcl;
  uint16_t ramar;
  uint8_t ramdr;

  uint8_t cache[512];
  bool cache_valid[32];
  SuperFxPixelCache pixel[2];

  uint64_t master_clock;
  int64_t clock_credit;
  bool irq_pending;

  uint64_t instruction_count;
  SuperFxTraceEntry trace[256];

  /* Optional presentation-only enhancement state. Architectural GSU state
   * above remains authoritative and always follows native hardware behavior. */
  SuperFxEnhancementMode enhancement_mode;
  uint8_t *ws_pixels;
  uint8_t *ws_valid;
  uint8_t *ws_present_pixels;
  uint8_t *ws_present_valid;
  void *ws_task_state;
  uint8_t *ws_task_ram;
  uint16_t ws_width, ws_extra;
  uint8_t ws_height;
  bool ws_render_active, ws_replay_pending, ws_replay_mode, ws_frame_ready;
  bool ws_pending_ready;
  uint8_t ws_replay_zero_word_count;
  uint16_t ws_replay_zero_words[8];
  uint16_t ws_saved_center_x, ws_saved_max_x;
  uint16_t ws_last_task, ws_task_address;
  uint16_t ws_center_ram, ws_max_ram;
  uint8_t ws_task_pbr;
} SuperFx;

SuperFx *superfx_create(uint8_t *rom, uint32_t rom_size,
                        uint8_t *ram, uint32_t ram_size);
void superfx_destroy(SuperFx *fx);
void superfx_reset(SuperFx *fx);

/* Synchronize to the S-CPU's monotonically increasing SNES master clock. */
void superfx_sync(SuperFx *fx, uint64_t master_clock);

uint8_t superfx_cpu_read_io(SuperFx *fx, uint16_t address);
void superfx_cpu_write_io(SuperFx *fx, uint16_t address, uint8_t data);
uint8_t superfx_cpu_read_rom(SuperFx *fx, uint32_t address, uint8_t open_bus);
uint8_t superfx_cpu_read_ram(SuperFx *fx, uint32_t address, uint8_t open_bus);
void superfx_cpu_write_ram(SuperFx *fx, uint32_t address, uint8_t data);

/* Select a host-side presentation enhancement. Newly created cores default
 * to None. Changing modes discards all queued/presented enhanced frames. */
void superfx_set_enhancement_mode(SuperFx *fx,
                                  SuperFxEnhancementMode mode);
SuperFxEnhancementMode superfx_get_enhancement_mode(const SuperFx *fx);

/* Configure a presentation-only wider replay for a GSU rendering task.
 * Configuration is inert unless WidescreenLinearProjection was explicitly
 * selected above. The task's projection center and maximum X are supplied as
 * GSU RAM offsets, keeping title-specific addresses out of the LLE core.
 * `extra` is the added projected width per side; zero disables it. */
void superfx_set_widescreen(SuperFx *fx, uint16_t extra, uint8_t task_pbr,
                            uint16_t task_address, uint16_t center_x_ram,
                            uint16_t max_x_ram, uint8_t height);
/* Clear title-selected word flags only in the presentation replay. This lets
 * callers exclude screen-space HUD/effect subpasses from a task while keeping
 * the authoritative native task unchanged. */
void superfx_set_widescreen_replay_zero_words(SuperFx *fx,
                                              const uint16_t *ram_offsets,
                                              unsigned count);
bool superfx_get_widescreen_frame(const SuperFx *fx, const uint8_t **pixels,
                                  const uint8_t **valid, unsigned *width,
                                  unsigned *height);
/* Promote the most recently completed replay after the current PPU picture
 * has consumed the previously presented generation. */
void superfx_latch_widescreen_frame(SuperFx *fx);

#endif
