#pragma once

/*
 * host_main.h — the framework's desktop host, as a linkable unit.
 *
 * A game repo's main.c is a thin shim: it fills in a SnesDesktopHostGame
 * (identity, and the hooks this particular title needs) and calls
 * snesrecomp_desktop_main(). Everything a port used to carry in its own
 * 2,000-line main.c -- the pre-boot launcher, ROM resolution and digest
 * checks, config.ini and keybinds.ini, the window and the SDL / OpenGL
 * presenters, audio, gamepads, the in-game save-state browser and rewind
 * filmstrip, the OSD, the pacing clock, crash handlers and the post-mortem
 * report, the scripted-input harness -- lives here, and reaches every port
 * by pulling. The same shape as snesrecomp_target_post_mortem() and the
 * other snesrecomp_target_* helpers, one level up: snesrecomp_target_desktop_host().
 *
 * Why a unit and not an include: tools/new_project copies its main.c template
 * into the new repo at scaffold time, so a fix to the template reached no
 * project that already existed. Two ports scaffolded a week apart from the
 * same framework had different hosts, and neither could get the other's
 * fixes without a hand merge. mmx23_host_main.inc solved the same problem
 * for the two Mega Man X hosts by being included into each, parameterized by
 * macros; this is that idea with a struct instead of macros, so a port can be
 * built against it without defining anything at all.
 *
 * Derived from SuperMetroidRecomp's src/main.c (2026-09), itself a descendant
 * of the MMX host, with what the new-project template had that it lacked:
 * mod packages, Generate & rebuild, the netplay barrier, --launcher /
 * --no-launcher, and the ROM-beside-the-exe resolution.
 *
 * The host DEFINES the following symbols the framework leaves to hosts, so a
 * shim must not: Die, RtlApuLock/RtlApuUnlock, g_spc_player, RtlDrawPpuFrame,
 * g_ws_active, g_ws_extra, g_new_ppu, MkDir, ChangeWindowScale. It does NOT
 * define debug_on_* (those belong with the trace build; see
 * host_contract.c in a scaffolded project).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RtlGameInfo;
struct SpcPlayer;
struct RecompLauncherCModProvider;

/* Per-frame numbers a game's after_run_frame hook may want. Wall-clock
 * figures are diagnostics only; nothing here samples or changes guest state. */
typedef struct SnesDesktopHostFrameStats {
  unsigned frame;            /* 1-based count of simulated frames so far */
  double run_seconds;        /* since the main loop started */
  double guest_seconds;      /* wall time RtlRunFrame took for this frame */
  int audio_output_rate;     /* device rate, 0 when audio is disabled */
} SnesDesktopHostFrameStats;

typedef struct SnesDesktopHostGame {
  /* ── Identity ─────────────────────────────────────────────────────────── */
  const char *display_name;      /* "Super Metroid" (report header, launcher) */
  const char *window_title;      /* NULL: "<display_name> (Recompiled)" */
  const char *launcher_title;    /* NULL: "<display_name> — Launcher" */
  const char *region;            /* "JPN" (launcher subtitle); NULL: none */
  const char *rom_file;          /* SNESRECOMP_ROM_FILE: a copy beside the exe
                                    is picked up without a prompt; NULL: skip */
  const char *expected_sha256_hex; /* 64 hex chars, or NULL/"" = cannot verify */
  const char *expected_crc32_hex;  /* 8 hex chars, or NULL/"" */
  const char *game_id;           /* mod-package target id; NULL: mods off */
  const char *build_version;     /* "dev" or the release stamp */
  const char *env_prefix;        /* "SM": SM_RUN_FRAMES etc. are honoured as
                                    fallbacks for SNESRECOMP_RUN_FRAMES; NULL:
                                    only the SNESRECOMP_ spellings */
  const char *sram_path;         /* "saves/save.srm" shows the launcher's
                                    SAVES panel; NULL: no battery SRAM */
  const char *default_config_ini;/* written when no config.ini exists; NULL:
                                    the framework's default text */
  const struct RtlGameInfo *game_info;  /* required */
  int num_players;               /* 1..8; 0 = 1 */
  int debug_port;                /* TCP debug server; 0 = 4377 */
  int widescreen_supported;      /* launcher: show the Widescreen controls */
  int msu1_supported;            /* launcher: show the MSU-1 panel */
  double simulation_hz;          /* 0 = NTSC (60.0988 Hz) */
  /* Guest-side polling that decides the default frame width. Left at 0 the
   * host presents 256x224. */
  int frame_width, frame_height;
  /* Use the runner's widened PPU field instead of a game-owned compositor. */
  int native_widescreen;
  /* F7/F8 menus, with legacy slot 7/8 loads moved to F11/F12. */
  int state_menu_hotkeys;

  /* ── Hooks. Every one is optional. ────────────────────────────────────── */

  /* Build the title's SPC player (audio upload interception); NULL leaves
   * g_spc_player NULL and the guest's own APU upload path untouched. */
  struct SpcPlayer *(*create_spc_player)(void);

  /* Launcher Mods page. When the project builds with mod packages
   * (SNESRECOMP_ENABLE_MODS) the framework's mod runtime provider is used
   * unless this returns one, in which case this one wins. */
  const struct RecompLauncherCModProvider *(*mods_provider)(void);

  /* After config.ini (+config.local.ini) is parsed and before the launcher
   * opens. Load per-title settings here. */
  void (*after_config)(void);
  /* The ROM bytes just read, before SnesInit. */
  void (*on_rom_loaded)(const uint8_t *rom, size_t size);
  /* The guest's timeline just jumped: reset, save-state load, an overlay
   * closed after freezing it. Presenters drop interpolation history here. */
  void (*on_reset)(void);
  /* Immediately before RtlRunFrame. */
  void (*before_run_frame)(void);
  /* Immediately after RtlRunFrame (guest state is at a frame boundary). */
  void (*after_run_frame)(const SnesDesktopHostFrameStats *stats);
  /* Return nonzero while the guest is in a phase whose wall-time debt the
   * pacing clock should PRESERVE (see host_clock.h). Default: never. */
  int (*keep_pacing_debt)(void);

  /* ── Presenter. All optional; the defaults present the PPU field as is. ── */

  /* Decide this frame's dimensions from the window's drawable size. */
  void (*prepare_frame)(int drawable_w, int drawable_h, int *frame_w, int *frame_h);
  /* Around the game's draw_ppu_frame (the raster). `field` is the PPU's
   * 256-wide ARGB output, pitch 256*4. */
  void (*begin_sim_frame)(unsigned number);
  void (*end_sim_frame)(const uint8_t *field, unsigned number);
  /* Compose the frame to present. Return nonzero after writing frame_w x
   * frame_h ARGB pixels into dst; return 0 to have the host copy the PPU
   * field. `alpha` interpolates between the last two simulated frames when
   * the presentation clock runs faster than the simulation (else 1). */
  int (*draw_frame)(uint8_t *dst, size_t pitch, const uint8_t *field,
                    int frame_w, int frame_h, double alpha);
  /* Presentation rate. Return 0 to present every simulated frame at the
   * simulation rate (the default). Return a rate to decouple presentation:
   * the host then re-presents the captured frame between simulated ones,
   * calling draw_frame with a fractional alpha. Called with the display's
   * refresh rate, or 0 when the host only asks whether decoupling is wanted
   * (before a window exists). */
  double (*presentation_hz)(double display_refresh);
  /* Window size at scale 1 for a frame of this width (default: 4:3 on 224). */
  int (*window_base_width)(int frame_w);
  int (*window_base_height)(void);
} SnesDesktopHostGame;

/* The whole program. Returns the process exit code. */
int snesrecomp_desktop_main(const SnesDesktopHostGame *game, int argc, char **argv);

/* The last simulated frame's width, as decided by prepare_frame (256 by
 * default). For per-title code that composes against the current frame. */
int snesrecomp_desktop_frame_width(void);
int snesrecomp_desktop_frame_height(void);
void snesrecomp_desktop_set_widescreen(int enabled);

/* Ask the host to reset its pacing clock at the next opportunity (a title
 * that just changed its presentation settings). */
void snesrecomp_desktop_request_clock_reset(void);

#ifdef __cplusplus
}
#endif
