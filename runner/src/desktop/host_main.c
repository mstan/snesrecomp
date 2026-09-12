/*
 * host_main.c — the framework's desktop host. See host_main.h.
 *
 * Ported from SuperMetroidRecomp/src/main.c, which was the most complete host
 * in the ecosystem at the time (in-game overlays with a frozen-frame present,
 * the decoupled presentation clock, the scripted-input harness, the crash
 * pipeline), generalized through the SnesDesktopHostGame descriptor, and
 * given what the new-project template had that it lacked (mod packages,
 * Generate & rebuild, the netplay barrier, --launcher / --no-launcher, ROM
 * beside the executable). Behaviour a port relied on is preserved exactly;
 * where this file differs from that main.c the comment says why.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <signal.h>

#include "debug_server.h"
#include "desktop/sdl_compat.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef SYSTEM_VOLUME_MIXER_AVAILABLE
#define SYSTEM_VOLUME_MIXER_AVAILABLE 0
#endif
#if SYSTEM_VOLUME_MIXER_AVAILABLE
#include "platform/win32/volume_control.h"
#endif

#include "host_main.h"
#include "host_clock.h"

#include "snes/ppu.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/snes.h"
#include "snes/ws_shadow.h"
#include "audio_trace.h"

#include "types.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "common_cpu_infra.h"
#include "framedump.h"
#include "config.h"
#include "display_aspect.h"
#include "crc32.h"
#include "util.h"
#include "spc_player.h"
#include "launcher.h"
#include "launcher_cache.h"
#include "host_paths.h"
#include "keybinds.h"
#include "host_report.h"
#include "post_mortem.h"
#include "widescreen.h"
#include "snes_savestate_menu.h"
#include "snes_rewind.h"
#include "snes_overlay_draw.h"
#include "snes_osd.h"

#if SNESRECOMP_ENABLE_MODS
#include "mod_runtime.h"
#endif

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"   /* recomp_launcher_run_window() */
#include "launcher_profile.h"  /* launcher_profile_apply("snes", &gi) */
/* Generate & rebuild is wired when the project compiles the framework's
 * codegen host (the template's launcher block does; see CMakeLists.txt.in).
 * The header lives in snesrecomp/host, which only that block puts on the
 * include path, hence the probe. */
#if defined(__has_include)
#if __has_include("snesrecomp_codegen_host.h")
#include "snesrecomp_codegen_host.h"
#define SNESRECOMP_HOST_HAS_CODEGEN 1
#endif
#endif
#endif

#if defined(SNES_HAS_LOBBY_CLIENT)
/* Delay-sync netplay + lobby. Defined by snesrecomp_enable_recomp_net(); without
 * it every netplay block compiles out and the launcher's netplay button stays
 * hidden. */
#include "snes_netplay.h"
#include "snes_host_lobby.h"
#include "snes_host_app.h"
#if defined(SNESRECOMP_NET_ROLLBACK)
#include "netplay/snes_netplay_rb.h"
#endif
#endif

/* ─────────────────────────────────────────────────────────────────────────── */

static const SnesDesktopHostGame *g_game;
static char g_window_title[128];
static char g_launcher_title[160];

typedef struct GamepadInfo {
  uint32 modifiers;
  SDL_JoystickID joystick_id;
  SDL_Joystick *joystick;      /* raw (unmapped) joystick, else NULL */
  bool raw_joystick;
  uint8 index;
  uint8 axis_buttons;
  uint16 last_cmd[kGamepadBtn_Count];
  Sint16 last_axis_x, last_axis_y;
} GamepadInfo;

#if SNESRECOMP_SDL3
static void SDLCALL AudioStreamCallback(
    void *userdata, SDL_AudioStream *stream, int additional_amount,
    int total_amount);
#else
static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len);
#endif
static void SwitchDirectory(void);
static void EnsureConfigIniNextToExe(const char *exe_path);
static void RenderNumber(uint8 *dst, size_t pitch, int n, uint8 big);
static void OpenOneGamepad(int i);
static void OpenOneJoystick(int i);
static uint32 GetActiveControllers(void);
static void HandleVolumeAdjustment(int volume_adjustment);
static void HandleGamepadAxisInput(GamepadInfo *gi, int axis, Sint16 value);
static int RemapSdlButton(int button);
static void HandleGamepadInput(GamepadInfo *gi, int button, bool pressed);
static void HandleInput(int keyCode, int keyMod, bool pressed);
static void HandleCommand(uint32 j, bool pressed);
#ifndef __ANDROID__
void OpenGLRenderer_Create(struct RendererFuncs *funcs);
#endif

/* ── Symbols the framework leaves to the host ─────────────────────────────── */

bool g_new_ppu = true;

/* Shared widescreen contract. This host never activates the PPU's own
 * widescreen (the guest stays at 256); a title that wants a wider picture
 * composes one through the draw_frame hook. */
bool g_ws_active = false;
int g_ws_extra = 0;

struct SpcPlayer *g_spc_player;

/* The PPU rasterizes into this, not straight into the host texture: the
 * texture is only mapped for the instant of the present, while the line
 * renderer needs a stable target for the whole field (and the raster IRQ
 * runs guest code in the middle of it). Sized for the runner's maximum
 * side-space budget. */
static uint8_t g_my_pixels[kPpuBufWidth * 4 * 240];

extern uint8_t g_ram[0x20000];

enum {
  kDefaultFullscreen = 0,
  kMaxWindowScale = 10,
  kDefaultFreq = 44100,
  kDefaultChannels = 2,
  kDefaultSamples = 2048,
};

static int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decode the digests the build generated from rom_identity.txt. Returns 0
 * when the identity carries none, which callers must read as "cannot verify"
 * rather than "verified". */
static int DecodeRomIdentity(uint8_t sha_out[32], uint32_t *crc_out) {
  const char *sha = g_game->expected_sha256_hex;
  const char *crc = g_game->expected_crc32_hex;
  if (!sha || strlen(sha) != 64 || !crc || strlen(crc) != 8)
    return 0;
  for (int i = 0; i < 32; i++) {
    int hi = HexNibble(sha[i * 2]), lo = HexNibble(sha[i * 2 + 1]);
    if (hi < 0 || lo < 0) return 0;
    sha_out[i] = (uint8_t)((hi << 4) | lo);
  }
  uint32_t v = 0;
  for (int i = 0; i < 8; i++) {
    int n = HexNibble(crc[i]);
    if (n < 0) return 0;
    v = (v << 4) | (uint32_t)n;
  }
  *crc_out = v;
  return 1;
}

/* Environment knobs: SNESRECOMP_<name>, then <env_prefix>_<name> so a port's
 * existing scripts and CI keep working after it adopts this host. */
static const char *HostGetenv(const char *name) {
  char buf[96];
  const char *v;
  snprintf(buf, sizeof(buf), "SNESRECOMP_%s", name);
  v = getenv(buf);
  if (v && *v) return v;
  if (g_game->env_prefix && *g_game->env_prefix) {
    snprintf(buf, sizeof(buf), "%s_%s", g_game->env_prefix, name);
    v = getenv(buf);
    if (v && *v) return v;
  }
  return NULL;
}

static uint32 g_win_flags = SDL_WINDOW_RESIZABLE;
static SDL_Window *g_window;

static uint8 g_paused, g_turbo, g_cursor = true;
static uint8 g_current_window_scale;
static uint32 g_input_state;
/* Gamepad-driven SNES controller bits, kept separate from g_input_state
 * (keyboard) so the per-frame keybinds.ini polling at the top of the
 * main loop doesn't clear bits the gamepad just set. OR'd into `inputs`
 * once per frame alongside g_input_state and axis_buttons. */
static uint32 g_pad_buttons;
static bool g_display_perf;
static int g_curr_fps;
static int g_ppu_render_flags = 0;
static int g_snes_width = 256, g_snes_height = 224;
static double g_present_alpha = 1;
static bool g_reset_clock;
static double g_simulation_hz = SNES_HOST_NTSC_HZ;

int snesrecomp_desktop_frame_width(void) { return g_snes_width > 0 ? g_snes_width : 256; }
int snesrecomp_desktop_frame_height(void) { return g_snes_height > 0 ? g_snes_height : 224; }
void snesrecomp_desktop_request_clock_reset(void) { g_reset_clock = true; }

static double MonotonicSeconds(void) {
#if defined(__linux__)
  /* Match the clock used by Linux sleep/audio scheduling. SDL's performance
   * counter uses MONOTONIC_RAW, which excludes frequency corrections: on a
   * drifting VM it can pace the guest slower than the audio device. */
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#endif
  return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
}

/* Opt-in wall-time diagnostics (SNESRECOMP_HOST_PROFILE=1). No guest state is
 * sampled or changed here. Values include preemption/lock waits and are not
 * CPU-time measurements. */
enum { kProfileGuest, kProfileRaster, kProfileAcquire,
       kProfileCompose, kProfilePresent, kProfileTrace, kProfileGameHook,
       kProfileEvents, kProfileWait, kProfileCount };
static bool g_profile;
static unsigned g_profile_frame;
static struct { double total, maximum; unsigned count, maximum_frame; } g_timings[kProfileCount];
static double ProfileStart(void) {
  return g_profile ? MonotonicSeconds() : 0;
}
static void ProfileEnd(unsigned stage, double start) {
  if (!g_profile) return;
  double elapsed = MonotonicSeconds() - start;
  g_timings[stage].total += elapsed;
  if (elapsed > g_timings[stage].maximum) {
    g_timings[stage].maximum = elapsed;
    g_timings[stage].maximum_frame = g_profile_frame;
  }
  ++g_timings[stage].count;
}
static void WaitUntil(double deadline) {
  double profile_start = ProfileStart();
  /* Short deadline wait: a fixed 1ms sleep on every presentation-only
   * iteration unnecessarily overshoots near deadlines. */
  double now = MonotonicSeconds();
  while (now < deadline) {
    double remaining_ms = (deadline - now) * 1000;
    if (remaining_ms > 1.5)
      SDL_Delay((Uint32)(remaining_ms - 0.5));
    else
      SDL_Delay(0);
    now = MonotonicSeconds();
  }
  ProfileEnd(kProfileWait, profile_start);
}
static double DisplayRefresh(void) {
  if (!g_window) return 60;
#if SNESRECOMP_SDL3
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
  return mode ? mode->refresh_rate : 60;
#else
  SDL_DisplayMode mode;
  return SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_window), &mode) == 0
      ? mode.refresh_rate : 60;
#endif
}
/* The presentation rate the title wants right now: 0 = lockstep with the
 * simulation. */
static double WantedPresentationHz(double refresh) {
  if (!g_game->presentation_hz) return 0;
  double hz = g_game->presentation_hz(refresh);
  return hz > 0 ? hz : 0;
}
static bool PresentationDecoupled(void) { return WantedPresentationHz(0) > 0; }

static int WindowBaseWidth(int frame_w) {
  if (g_game->window_base_width) return g_game->window_base_width(frame_w);
  /* 4:3 on a 240-line window: 256 -> 320. */
  return (frame_w * 5 + 2) / 4;
}
static int WindowBaseHeight(void) {
  if (g_game->window_base_height) return g_game->window_base_height();
  return 240;
}

static int g_last_drawable_width, g_last_drawable_height;
static const char *g_active_config_file;
static int g_sdl_audio_mixer_volume = SNESRECOMP_SDL_MIX_MAXVOLUME;

static struct RendererFuncs g_renderer_funcs;

/* Set by the hotkeys; consumed once in the frame loop. */
static int g_savestate_menu_hotkey;
static int g_rewind_hotkey;

/* The last field actually presented, kept so an overlay can freeze the guest
 * and still have something to draw behind itself. Sized like g_my_pixels. */
static uint8_t g_frozen_frame[kPpuBufWidth * 4 * 240];
static int g_frozen_w, g_frozen_h;
/* The simulated frame the next present shows (for SNESRECOMP_SCREENSHOT). */
static unsigned g_screenshot_frame;

static GamepadInfo g_gamepad[2];

extern Snes *g_snes;

static void GameReset(void) {
  if (g_game->on_reset) g_game->on_reset();
  g_reset_clock = true;
}

static void PreparePpuFrame(void) {
  int drawable_width = 0, drawable_height = 0;
  if (g_renderer_funcs.GetOutputSize)
    g_renderer_funcs.GetOutputSize(&drawable_width, &drawable_height);
  if (drawable_width <= 0 || drawable_height <= 0)
    SDL_GetWindowSize(g_window, &drawable_width, &drawable_height);
  if (drawable_width > 0 && drawable_height > 0) {
    g_last_drawable_width = drawable_width;
    g_last_drawable_height = drawable_height;
  } else {
    drawable_width = g_last_drawable_width;
    drawable_height = g_last_drawable_height;
  }

  int fw = g_game->frame_width > 0 ? g_game->frame_width : 256;
  int fh = g_game->frame_height > 0 ? g_game->frame_height : 224;
  if (g_game->prepare_frame)
    g_game->prepare_frame(drawable_width, drawable_height, &fw, &fh);
  if (fw <= 0 || fw > kPpuBufWidth) fw = 256;
  if (fh <= 0 || fh > 240) fh = 224;
  g_snes_width = fw;
  g_snes_height = fh;
  /* The PPU's own widescreen never activates here. */
  g_ws_extra = 0;
  g_ws_active = false;
  g_new_ppu = (g_ppu_render_flags & kPpuRenderFlags_NewRenderer) != 0;
  if (g_config.no_sprite_limits)
    g_ppu_render_flags |= kPpuRenderFlags_NoSpriteLimits;
  else
    g_ppu_render_flags &= ~kPpuRenderFlags_NoSpriteLimits;
  PpuBeginDrawing(g_ppu, g_my_pixels, 256 * 4, 0);
}

// --- Scripted input ---
typedef struct {
  uint32 mask;      // button bits to hold
  int hold_frames;  // frames to hold mask (0 = release)
  int wait_frames;  // frames to wait after hold ends before next entry
  uint32 poke_addr; // script-only WRAM write address
  uint8 *poke_bytes;
  int poke_count;
} ScriptEntry;

typedef struct {
  uint32 addr;
  uint8 *bytes;
  int count;
} ScriptForcePoke;

static ScriptEntry *g_script_entries;
static int g_script_count;
static int g_script_index;    // current entry
static int g_script_phase;    // 0=holding, 1=waiting
static int g_script_counter;  // frames left in current phase
static ScriptForcePoke *g_script_force_pokes;
static int g_script_force_poke_count;
static int g_script_force_poke_cap;

static uint32 ParseButtonMask(const char *name) {
  const char *sep = strpbrk(name, "+,|");
  if (sep) {
    uint32 mask = 0;
    const char *p = name;
    while (*p) {
      size_t len = strcspn(p, "+,|");
      char part[32];
      if (len == 0 || len >= sizeof(part))
        return 0;
      memcpy(part, p, len);
      part[len] = 0;
      mask |= ParseButtonMask(part);
      p += len;
      if (*p)
        p++;
    }
    return mask;
  }

  if (strcmp(name, "start")  == 0) return 0x0008;
  if (strcmp(name, "select") == 0) return 0x0004;
  if (strcmp(name, "up")     == 0) return 0x0010;
  if (strcmp(name, "down")   == 0) return 0x0020;
  if (strcmp(name, "left")   == 0) return 0x0040;
  if (strcmp(name, "right")  == 0) return 0x0080;
  if (strcmp(name, "a")      == 0) return 0x0100;
  if (strcmp(name, "b")      == 0) return 0x0001;
  if (strcmp(name, "x")      == 0) return 0x0200;
  if (strcmp(name, "y")      == 0) return 0x0002;
  if (strcmp(name, "l")      == 0) return 0x0400;
  if (strcmp(name, "r")      == 0) return 0x0800;
  fprintf(stderr, "script: unknown button '%s'\n", name);
  return 0;
}

static int ParseHexByte(const char *s, uint8 *out) {
  int hi = HexNibble(s[0]), lo = HexNibble(s[1]);
  if (hi < 0 || lo < 0)
    return 0;
  *out = (uint8)((hi << 4) | lo);
  return 1;
}

static uint8 *ParseHexBytes(uint32 addr, const char *hex, int *out_count) {
  size_t hex_len = strlen(hex);
  int byte_count = (int)(hex_len / 2);
  if ((hex_len & 1) || byte_count <= 0 || addr + byte_count > 0x20000u)
    return NULL;

  uint8 *bytes = (uint8 *)malloc((size_t)byte_count);
  if (!bytes)
    return NULL;
  for (int i = 0; i < byte_count; i++) {
    if (!ParseHexByte(hex + i * 2, &bytes[i])) {
      free(bytes);
      return NULL;
    }
  }
  *out_count = byte_count;
  return bytes;
}

static void AddScriptForcePoke(uint32 addr, uint8 *bytes, int count) {
  if (g_script_force_poke_count >= g_script_force_poke_cap) {
    g_script_force_poke_cap = g_script_force_poke_cap
        ? g_script_force_poke_cap * 2 : 8;
    g_script_force_pokes = (ScriptForcePoke *)realloc(
        g_script_force_pokes,
        (size_t)g_script_force_poke_cap * sizeof(ScriptForcePoke));
  }
  ScriptForcePoke *p = &g_script_force_pokes[g_script_force_poke_count++];
  p->addr = addr;
  p->bytes = bytes;
  p->count = count;
}

static void ApplyScriptForcePokes(void) {
  for (int i = 0; i < g_script_force_poke_count; i++) {
    ScriptForcePoke *p = &g_script_force_pokes[i];
    if (p->bytes && p->count > 0 &&
        p->addr + (uint32)p->count <= 0x20000u)
      memcpy(g_ram + p->addr, p->bytes, (size_t)p->count);
  }
}

static ScriptEntry *NewScriptEntry(int *cap) {
  if (g_script_count >= *cap) {
    *cap *= 2;
    g_script_entries = (ScriptEntry *)realloc(g_script_entries, (size_t)*cap * sizeof(ScriptEntry));
  }
  ScriptEntry *e = &g_script_entries[g_script_count++];
  memset(e, 0, sizeof(*e));
  return e;
}

/* Script grammar, one command per line, `#` comments:
 *   wait N                  frames before the next command
 *   press <buttons> [N]     hold a+b+... for N frames (default 1)
 *   loadstate N             load save-state slot N
 *   poke <addr> <hex>       write WRAM bytes for one frame
 *   pokefor <addr> <hex> N  write WRAM bytes for N frames
 *   forcepoke <addr> <hex>  write WRAM bytes every frame from now on */
static void LoadScript(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "script: cannot open '%s'\n", path); return; }

  int cap = 64;
  g_script_entries = (ScriptEntry *)malloc(cap * sizeof(ScriptEntry));
  g_script_count = 0;

  char line[256];
  // pending wait accumulates between press commands
  int pending_wait = 0;
  while (fgets(line, sizeof(line), f)) {
    // strip comment and newline
    char *c = strchr(line, '#'); if (c) *c = 0;
    char cmd[64], arg1[64];
    int n = 0;
    if (sscanf(line, "%63s %63s %d", cmd, arg1, &n) < 1) continue;
    if (strcmp(cmd, "wait") == 0) {
      int frames = (sscanf(line, "%*s %d", &n) == 1) ? n : 0;
      pending_wait += frames;
    } else if (strcmp(cmd, "loadstate") == 0) {
      int slot = 0;
      sscanf(line, "%*s %d", &slot);
      ScriptEntry *e = NewScriptEntry(&cap);
      e->mask = 0x80000000 | (slot & 0xF);  // special flag: high bit = loadstate
      e->hold_frames = 1;
      e->wait_frames = pending_wait;
      pending_wait = 0;
    } else if (strcmp(cmd, "forcepoke") == 0) {
      unsigned addr = 0;
      char hex[256] = {0};
      if (sscanf(line, "%*s %x %255s", &addr, hex) != 2)
        continue;
      int byte_count = 0;
      uint8 *bytes = ParseHexBytes(addr, hex, &byte_count);
      if (!bytes)
        continue;
      ScriptEntry *e = NewScriptEntry(&cap);
      e->mask = 0x20000000;  // special flag: persistent WRAM poke
      e->hold_frames = 1;
      e->wait_frames = pending_wait;
      e->poke_addr = addr;
      e->poke_bytes = bytes;
      e->poke_count = byte_count;
      pending_wait = 0;
    } else if (strcmp(cmd, "poke") == 0 || strcmp(cmd, "pokefor") == 0) {
      unsigned addr = 0;
      char hex[256] = {0};
      int hold = 1;
      int matched = strcmp(cmd, "pokefor") == 0
          ? sscanf(line, "%*s %x %255s %d", &addr, hex, &hold)
          : sscanf(line, "%*s %x %255s", &addr, hex);
      if (matched < 2)
        continue;
      if (hold < 1)
        hold = 1;
      int byte_count = 0;
      uint8 *bytes = ParseHexBytes(addr, hex, &byte_count);
      if (!bytes)
        continue;
      ScriptEntry *e = NewScriptEntry(&cap);
      e->mask = 0x40000000;  // special flag: WRAM poke
      e->hold_frames = hold;
      e->wait_frames = pending_wait;
      e->poke_addr = addr;
      e->poke_bytes = bytes;
      e->poke_count = byte_count;
      pending_wait = 0;
    } else if (strcmp(cmd, "press") == 0) {
      int hold = (sscanf(line, "%*s %*s %d", &n) == 1) ? n : 1;
      ScriptEntry *e = NewScriptEntry(&cap);
      e->mask = ParseButtonMask(arg1);
      e->hold_frames = hold;
      e->wait_frames = pending_wait;
      pending_wait = 0;
    } else {
      fprintf(stderr, "script: unknown command '%s'\n", cmd);
    }
  }
  fclose(f);

  if (g_script_count > 0) {
    g_script_index = 0;
    g_script_phase = 1; // start with the wait_frames of first entry
    g_script_counter = g_script_entries[0].wait_frames;
    fprintf(stderr, "script: loaded %d entries from '%s'\n", g_script_count, path);
  }
}

static uint32 TickScript(void) {
  ApplyScriptForcePokes();

  if (!g_script_entries || g_script_index >= g_script_count)
    return 0;

  ScriptEntry *e = &g_script_entries[g_script_index];

  if (g_script_phase == 1) {
    // waiting
    if (g_script_counter > 0) { g_script_counter--; return 0; }
    // done waiting — start hold
    g_script_phase = 0;
    g_script_counter = e->hold_frames;
  }

  if (g_script_phase == 0) {
    if (g_script_counter > 0) {
      g_script_counter--;
      if (e->mask & 0x80000000) {
        RtlSaveLoad(kSaveLoad_Load, e->mask & 0xF);
        GameReset();
        return 0;
      }
      if (e->mask & 0x40000000) {
        if (e->poke_bytes && e->poke_count > 0 &&
            e->poke_addr + (uint32)e->poke_count <= 0x20000u)
          memcpy(g_ram + e->poke_addr, e->poke_bytes, (size_t)e->poke_count);
        return 0;
      }
      if (e->mask & 0x20000000) {
        if (e->poke_bytes && e->poke_count > 0)
          AddScriptForcePoke(e->poke_addr, e->poke_bytes, e->poke_count);
        return 0;
      }
      return e->mask;
    }
    // hold done — advance
    g_script_index++;
    if (g_script_index < g_script_count) {
      e = &g_script_entries[g_script_index];
      g_script_phase = 1;
      g_script_counter = e->wait_frames;
    }
    return 0;
  }
  return 0;
}

void NORETURN Die(const char *error) {
  /* Record the message before exiting: the atexit post-mortem dump
   * includes it and preserves a timestamped crash copy (see
   * host_report_has_fatal in post_mortem.c). */
  host_report_fatal(error);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g_window_title, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

static GamepadInfo *GetGamepadInfo(SDL_JoystickID id) {
  return (g_gamepad[0].joystick_id == id) ? &g_gamepad[0] :
    (g_gamepad[1].joystick_id == id) ? &g_gamepad[1] : NULL;
}

void ChangeWindowScale(int scale_step) {
  if ((SDL_GetWindowFlags(g_window) & (SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) != 0)
    return;
  int max_scale = kMaxWindowScale;
  SDL_Rect bounds;
  int bt = -1, bl, bb, br;
  /* Both return true-on-success in SDL3 (0-on-success in SDL2); the shims
   * normalise that, and taking the display from the window also matches SDL3's
   * DisplayID model. */
  if (snesrecomp_sdl_get_display_usable_bounds(g_window, &bounds)) {
    // this call may take a while before it is reported by Windows (or not at all in my testing)
    if (!snesrecomp_sdl_get_window_borders_size(g_window, &bt, &bl, &bb, &br)) {
      // guess based on Windows 10/11 defaults
      bl = br = bb = 1;
      bt = 31;
    }
    // Allow a scale level slightly above the max that fits on screen
    int logical_width = WindowBaseWidth(g_snes_width);
    int logical_height = WindowBaseHeight();
    int mw = (bounds.w - bl - br + logical_width / 4) / logical_width;
    int mh = (bounds.h - bt - bb + logical_height / 4) / logical_height;
    max_scale = IntMin(mw, mh);
  }
  int new_scale = IntMax(IntMin(g_current_window_scale + scale_step, max_scale), 1);
  g_current_window_scale = new_scale;
  int w = new_scale * WindowBaseWidth(g_snes_width);
  int h = new_scale * WindowBaseHeight();

  SDL_SetWindowSize(g_window, w, h);
  if (bt >= 0) {
    // Center the window on top of the mouse
    int mx, my;
    snesrecomp_sdl_get_global_mouse_state(&mx, &my);
    int wx = IntMax(IntMin(mx - w / 2, bounds.x + bounds.w - bl - br - w), bounds.x + bl);
    int wy = IntMax(IntMin(my - h / 2, bounds.y + bounds.h - bt - bb - h), bounds.y + bt);
    SDL_SetWindowPosition(g_window, wx, wy);
  } else {
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

#define RESIZE_BORDER 20
static SDL_HitTestResult HitTestCallback(SDL_Window *win, const SDL_Point *pt, void *data) {
  (void)data;
  uint32 flags = SDL_GetWindowFlags(win);
  if ((flags & SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 || (flags & SDL_WINDOW_FULLSCREEN) != 0)
    return SDL_HITTEST_NORMAL;

  if ((SDL_GetModState() & KMOD_CTRL) != 0)
    return SDL_HITTEST_DRAGGABLE;

  int w, h;
  SDL_GetWindowSize(win, &w, &h);

  if (pt->y < RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPLEFT :
      (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
  } else if (pt->y >= h - RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMLEFT :
      (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
  } else {
    if (pt->x < RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_LEFT;
    } else if (pt->x >= w - RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_RIGHT;
    }
  }
  return SDL_HITTEST_NORMAL;
}

/* Simulation owns this call: HDMA and the raster IRQ execute exactly once per
 * simulated frame even when that frame is not presented, and never for an
 * interpolated present. The game's draw_ppu_frame runs guest code (the
 * raster IRQ), which is why nothing else in this file may call it. */
static void CaptureSimulationFrame(unsigned number) {
  PreparePpuFrame();
  if (g_game->begin_sim_frame) g_game->begin_sim_frame(number);
  if (g_rtl_game_info && g_rtl_game_info->draw_ppu_frame)
    g_rtl_game_info->draw_ppu_frame();
  if (g_game->end_sim_frame) g_game->end_sim_frame(g_my_pixels, number);
}

void RtlDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  (void)render_flags;
  if (!pixel_buffer) return;
  if (g_game->draw_frame &&
      g_game->draw_frame(pixel_buffer, pitch, g_my_pixels, g_snes_width,
                         g_snes_height, g_present_alpha))
    return;
  RtlWidescreenPresent(pixel_buffer, pitch, g_my_pixels, g_snes_width, g_snes_height);
}

/* The OSD (FPS readout, turbo, save-slot toasts) composited into the frame.
 * The framework rasterizes it for a ~3x window; the frame is 1x, so it lands
 * at half size here and the SDL/GL scale brings it back. Window-space chrome
 * is not available through the RendererFuncs contract, which is what the GL
 * presenter speaks. */
static void ComposeOsd(uint8 *dst, int pitch, int dst_w, int dst_h, int scale_div) {
  const uint32_t *px = NULL;
  int w = 0, h = 0;
  if (snes_osd_image(&px, &w, &h) && px && w > 0 && h > 0)
    snes_ovl_blit_panel_rect(dst, pitch, dst_w, dst_h, px, w, h,
                             4 / scale_div, 4 / scale_div, w / scale_div, h / scale_div);
  snes_osd_present_done();
}

#ifdef ENABLE_ORACLE_BACKEND
/* Remap the runner's 12-bit per-player input word to the SNES hardware
 * joypad bit order the snes9x bridge expects. */
static uint16_t runner_to_snes_joypad(uint16_t r) {
  uint16_t s = 0;
  if (r & 0x001) s |= 0x8000; /* B      */
  if (r & 0x002) s |= 0x4000; /* Y      */
  if (r & 0x004) s |= 0x2000; /* SELECT */
  if (r & 0x008) s |= 0x1000; /* START  */
  if (r & 0x010) s |= 0x0800; /* UP     */
  if (r & 0x020) s |= 0x0400; /* DOWN   */
  if (r & 0x040) s |= 0x0200; /* LEFT   */
  if (r & 0x080) s |= 0x0100; /* RIGHT  */
  if (r & 0x100) s |= 0x0080; /* A      */
  if (r & 0x200) s |= 0x0040; /* X      */
  if (r & 0x400) s |= 0x0020; /* L      */
  if (r & 0x800) s |= 0x0010; /* R      */
  return s;
}
#endif

static void DrawPpuFrameWithPerf(void) {
  double profile_start = ProfileStart();
  PreparePpuFrame();
  const int render_scale = 1;
  uint8 *pixel_buffer = 0;
  int pitch = 0;

  g_renderer_funcs.BeginDraw(g_snes_width * render_scale,
                             g_snes_height * render_scale,
                             &pixel_buffer, &pitch);
  ProfileEnd(kProfileAcquire, profile_start);
  if (!pixel_buffer) {
    g_renderer_funcs.EndDraw();
    return;
  }
  profile_start = ProfileStart();
  if (g_display_perf || g_config.display_perf_title) {
    static float history[64], average;
    static int history_pos;
    uint64 before = SDL_GetPerformanceCounter();
    RtlDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
    uint64 after = SDL_GetPerformanceCounter();
    float v = (double)SDL_GetPerformanceFrequency() / (after - before);
    average += v - history[history_pos];
    history[history_pos] = v;
    history_pos = (history_pos + 1) & 63;
    g_curr_fps = average * (1.0f / 64);
  } else {
    RtlDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
  }
  if (g_display_perf)
    RenderNumber(pixel_buffer + pitch * render_scale, pitch, g_curr_fps, render_scale == 4);

  /* SNESRECOMP_SCREENSHOT=<path.ppm> [SNESRECOMP_SCREENSHOT_FRAME=<n>]: write
   * the frame presented at simulated frame n (default: the first) as a PPM.
   * The doctrine says screenshot before asserting anything about visible
   * state, and a headless run (SDL_VIDEODRIVER=dummy) has no other way to
   * produce one. A black-frame report is then a file, not a description. */
  {
    static int shot_done;
    static long shot_frame = -2;
    if (shot_frame == -2) {
      const char *v = HostGetenv("SCREENSHOT_FRAME");
      shot_frame = v ? strtol(v, NULL, 0) : 1;
    }
    const char *path = shot_done ? NULL : HostGetenv("SCREENSHOT");
    if (path && (long)g_screenshot_frame >= shot_frame) {
      FILE *f = fopen(path, "wb");
      if (f) {
        const int w = g_snes_width * render_scale, h = g_snes_height * render_scale;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) {
          const uint32_t *row = (const uint32_t *)(pixel_buffer + (size_t)y * (size_t)pitch);
          for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            fputc((p >> 16) & 0xFF, f); fputc((p >> 8) & 0xFF, f); fputc(p & 0xFF, f);
          }
        }
        fclose(f);
        host_report_breadcrumb("screenshot: wrote %s (%dx%d) at frame %u", path, w, h,
                               g_screenshot_frame);
      } else {
        host_report_breadcrumb("screenshot: cannot open %s", path);
      }
      shot_done = 1;
    }
  }

  /* Keep a copy of what was just presented. An overlay freezes the guest, and
   * the backdrop behind it has to come from somewhere that is NOT another
   * call to the game's draw_ppu_frame -- that runs guest code, so calling it
   * from a modal loop pushes an interrupt frame every 8ms into a guest that
   * is not executing. That is what locked Super Metroid up when the
   * save-state browser was opened. */
  {
    const int rows = g_snes_height * render_scale;
    const int row_bytes = g_snes_width * render_scale * 4;
    if (rows > 0 && row_bytes > 0 &&
        (size_t)rows * (size_t)row_bytes <= sizeof(g_frozen_frame)) {
      for (int y = 0; y < rows; y++)
        memcpy(g_frozen_frame + (size_t)y * (size_t)row_bytes,
               pixel_buffer + (size_t)y * (size_t)pitch, (size_t)row_bytes);
      g_frozen_w = g_snes_width * render_scale;
      g_frozen_h = rows;
    }
  }
  ComposeOsd(pixel_buffer, pitch, g_snes_width * render_scale,
             g_snes_height * render_scale, 2);

  ProfileEnd(kProfileCompose, profile_start);
  profile_start = ProfileStart();
  g_renderer_funcs.EndDraw();
  ProfileEnd(kProfilePresent, profile_start);
}

/* Seat-0 input word the overlays navigate with — the same sources the guest
 * gets, minus the script (an overlay is a human facility). */
static uint32 OverlayNavInputs(void) {
  return g_input_state | g_pad_buttons | g_gamepad[0].axis_buttons;
}

/* Present a frozen field with an overlay on top, WITHOUT running guest code.
 *
 * The draw buffer is requested at the panel's own resolution (512x448, twice
 * the SNES field) so the panel lands 1:1 and its text stays crisp, with the
 * frozen game upscaled behind it. Compositing into the 256-wide game buffer
 * instead halved the panel and made it noticeably coarser than the same
 * overlay looks in other ports.
 *
 * Placement is per-overlay, because the two modules draw for different
 * shapes: the save-state browser is an opaque full-rect panel, the rewind
 * filmstrip belongs across the bottom third of the frame, annotating the
 * moment it describes. */
static void PresentFrozenWithOverlay(void) {
  const uint32_t *panel = NULL;
  int pw = 0, ph = 0;
  int is_menu = snes_savestate_menu_overlay_image(&panel, &pw, &ph) && panel;
  if (!is_menu && !(snes_rewind_overlay_image(&panel, &pw, &ph) && panel))
    panel = NULL;

  const int draw_w = (panel && pw > 0) ? pw : g_snes_width;
  const int draw_h = (panel && ph > 0) ? ph : g_snes_height;
  uint8 *pixel_buffer = 0;
  int pitch = 0;

  g_renderer_funcs.BeginDraw(draw_w, draw_h, &pixel_buffer, &pitch);
  if (!pixel_buffer) {
    g_renderer_funcs.EndDraw();
    return;
  }

  if (g_frozen_w > 0 && g_frozen_h > 0)
    snes_ovl_upscale_frame(pixel_buffer, pitch, draw_w, draw_h,
                           (const uint32_t *)g_frozen_frame,
                           g_frozen_w * 4, g_frozen_w, g_frozen_h);
  else
    memset(pixel_buffer, 0, (size_t)draw_h * (size_t)pitch);

  if (panel) {
    if (is_menu) {
      snes_ovl_blit_panel_rect(pixel_buffer, pitch, draw_w, draw_h,
                               panel, pw, ph, 0, 0, draw_w, draw_h);
    } else {
      const int strip_h = draw_h / 3;
      snes_ovl_blit_panel_rect(pixel_buffer, pitch, draw_w, draw_h,
                               panel, pw, ph,
                               0, draw_h - strip_h, draw_w, strip_h);
    }
  }
  ComposeOsd(pixel_buffer, pitch, draw_w, draw_h, draw_w >= 512 ? 1 : 2);
  /* SNESRECOMP_OVERLAY_DUMP=<path>: write the composited overlay frame as a
   * PPM. The overlays can only be driven by a human, so this is the only way
   * to check that a panel actually reaches the screen rather than inferring
   * it from the module reporting itself open. */
  {
    const char *dump = HostGetenv("OVERLAY_DUMP");
    static int dumped = 0;
    if (dump && !dumped && panel) {
      FILE *f = fopen(dump, "wb");
      if (f) {
        fprintf(f, "P6\n%d %d\n255\n", draw_w, draw_h);
        for (int y = 0; y < draw_h; y++) {
          const uint32_t *row = (const uint32_t *)(pixel_buffer + (size_t)y * (size_t)pitch);
          for (int x = 0; x < draw_w; x++) {
            uint32_t p = row[x];
            fputc((p >> 16) & 0xFF, f); fputc((p >> 8) & 0xFF, f); fputc(p & 0xFF, f);
          }
        }
        fclose(f);
        dumped = 1;
        fprintf(stderr, "[overlay_dump] wrote %s (%dx%d, %s)\n", dump, draw_w, draw_h,
                is_menu ? "save-state browser" : "rewind filmstrip");
      }
    }
  }

  g_renderer_funcs.EndDraw();
}

/* Save-state browser's modal pump. The guest is FROZEN throughout: this loop
 * never calls RtlRunFrame and never calls the game's draw_ppu_frame, which is
 * what makes "save right here" a definite point in time. */
static void RunSavestateMenuLoop(bool *running) {
  /* Always on, and deliberately so: the guest is about to stop, and from the
   * outside a deliberate freeze and a hang look identical. Whoever reads the
   * log next should not have to guess which one they got -- and the player
   * needs to be told which button leaves, because the answer is not Escape
   * on a pad. */
  unsigned frames = 0;
  host_report_breadcrumb("save-state browser OPEN - guest frozen until it "
                         "closes (pad B, or Escape/Backspace on the keyboard)");
  while (snes_savestate_menu_is_open() && *running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        *running = false;
        snes_savestate_menu_close();
        break;
      case SDL_KEYDOWN:
        /* Straight to the overlay, NOT through HandleInput: the game's own
         * hotkeys must not fire while a panel owns the screen (F1 would load
         * a state behind the browser that is asking which state to load). */
        snes_savestate_menu_handle_key(SNESRECOMP_SDL_EVENT_KEY(event),
                                       SNESRECOMP_SDL_EVENT_REPEAT(event));
        break;
      case SDL_KEYUP:
        /* Keep the keyboard's view of held keys honest so a direction held
         * across the close does not stick in the guest afterwards. */
        HandleInput(SNESRECOMP_SDL_EVENT_KEY(event),
                    SNESRECOMP_SDL_EVENT_MOD(event), false);
        break;
      }
    }
    snes_savestate_menu_poll_nav(OverlayNavInputs(), SDL_GetTicks());
    PresentFrozenWithOverlay();
    SDL_Delay(8);
    frames++;
  }
  host_report_breadcrumb("save-state browser CLOSED after %u pumps - guest resuming",
                         frames);
}

/* Rewind's modal pump. It needs its own: snes_rewind exposes step/commit/close
 * rather than the browser's handle_key/poll_nav. Controls match the other
 * ports: Left/Right scrub (hold to keep scrubbing), Enter or Space commits,
 * Escape cancels, and the pad mirrors them. */
static void RunRewindLoop(bool *running) {
  uint32 prev_pad = 0;
  uint32 held_dir = 0;
  uint32 held_since = 0, last_repeat = 0;
  unsigned frames = 0;
  host_report_breadcrumb("rewind filmstrip OPEN - guest frozen until it closes "
                         "(pad B, or Escape; Left/Right scrub, A or Enter commits)");
  while (snes_rewind_is_open() && *running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        *running = false;
        snes_rewind_close();
        break;
      case SDL_KEYDOWN:
        switch (SNESRECOMP_SDL_EVENT_KEY(event)) {
        case SDLK_LEFT:   snes_rewind_step(-1); break;
        case SDLK_RIGHT:  snes_rewind_step(+1); break;
        case SDLK_RETURN:
        case SDLK_SPACE:  snes_rewind_commit(); break;
        case SDLK_ESCAPE: snes_rewind_close();  break;
        default: break;
        }
        break;
      case SDL_KEYUP:
        HandleInput(SNESRECOMP_SDL_EVENT_KEY(event),
                    SNESRECOMP_SDL_EVENT_MOD(event), false);
        break;
      }
    }
    {
      /* Edge-triggered, with a hold-to-repeat: holding Left must not sprint
       * through the whole ring in a single pass of this loop. */
      const uint32 now = SDL_GetTicks();
      const uint32 pad = OverlayNavInputs();
      const uint32 pressed = pad & ~prev_pad;
      const uint32 dir = pad & (SNES_PAD_LEFT | SNES_PAD_RIGHT);
      if (pressed & SNES_PAD_LEFT)  snes_rewind_step(-1);
      if (pressed & SNES_PAD_RIGHT) snes_rewind_step(+1);
      if (pressed & SNES_PAD_A)     snes_rewind_commit();
      if (pressed & SNES_PAD_B)     snes_rewind_close();
      if (dir && dir != (SNES_PAD_LEFT | SNES_PAD_RIGHT)) {
        if (dir != held_dir) {
          held_dir = dir;
          held_since = now;
          last_repeat = now;
        } else if (now - held_since >= SNES_OVL_REPEAT_DELAY &&
                   now - last_repeat >= SNES_OVL_REPEAT_RATE) {
          snes_rewind_step((dir & SNES_PAD_LEFT) ? -1 : +1);
          last_repeat = now;
        }
      } else {
        held_dir = 0;
      }
      prev_pad = pad;
    }
    PresentFrozenWithOverlay();
    SDL_Delay(8);
    frames++;
  }
  host_report_breadcrumb("rewind filmstrip CLOSED after %u pumps - guest resuming",
                         frames);
  GameReset();
}

/* ── Audio ────────────────────────────────────────────────────────────────── */

static SDL_mutex *g_audio_mutex;
static uint8 *g_audiobuffer, *g_audiobuffer_cur, *g_audiobuffer_end;
static int g_frames_per_block;
static uint8 g_audio_channels;
static SDL_AudioDeviceID g_audio_device;
/* Only the thread executing guest work may wait for the audio consumer. */
static _Thread_local bool g_audio_producer_active;
static _Thread_local unsigned g_apu_lock_depth;
static _Thread_local bool g_audio_consumer_stalled;
static _Thread_local uint64_t g_audio_stalled_callback;
static uint64_t g_audio_callback_count;  /* protected by g_audio_mutex */
static bool g_audio_primed;  /* protected by g_audio_mutex */
#define HOST_AUDIO_PREFILL 2136u
#define HOST_AUDIO_HIGH_WATER 4096u
#if SNESRECOMP_SDL3
/* SDL3 replaced the pull callback with an SDL_AudioStream the app pushes into,
 * so the mixer needs a scratch buffer sized to whatever the stream asks for. */
static SDL_AudioStream *g_audio_stream;
static uint8 *g_audio_stream_buffer;
static size_t g_audio_stream_buffer_size;
#endif

void RtlApuLock(void) {
  SDL_LockMutex(g_audio_mutex);
  ++g_apu_lock_depth;
}

void RtlApuUnlock(void) {
  --g_apu_lock_depth;
  if (g_apu_lock_depth == 0 && g_audio_producer_active &&
      g_audio_consumer_stalled &&
      g_audio_callback_count != g_audio_stalled_callback)
    g_audio_consumer_stalled = false;
  if (g_apu_lock_depth == 0 && g_audio_producer_active &&
      !g_audio_consumer_stalled &&
      dsp_available(g_snes->apu->dsp) > HOST_AUDIO_HIGH_WATER) {
    /* Fast hosts can generate a multi-frame loader's PCM in milliseconds.
     * Let the device drain it before the bounded ring overflows. Always
     * release the mutex while waiting, and stop waiting if the device stalls. */
    double limit = MonotonicSeconds() + 0.25;
    while (dsp_available(g_snes->apu->dsp) > HOST_AUDIO_HIGH_WATER) {
      SDL_UnlockMutex(g_audio_mutex);
      SDL_Delay(1);
      SDL_LockMutex(g_audio_mutex);
      if (MonotonicSeconds() >= limit) {
        /* A disconnected device must not add this timeout to every frame.
         * Rearm only after the consumer has actually made progress. */
        g_audio_consumer_stalled = true;
        g_audio_stalled_callback = g_audio_callback_count;
        g_audio_producer_active = false;
        break;
      }
    }
  }
  SDL_UnlockMutex(g_audio_mutex);
}

/* Backend-agnostic mixer body. SDL2 calls it from its pull callback; SDL3 calls
 * it to fill a scratch buffer that is then pushed into the audio stream. */
static void FillAudioBuffer(Uint8 *stream, int len) {
  /* Boot-stage marker: proves the audio thread reached the mixer at
   * least once (the "crashed before the first sound" class of report). */
  static SDL_atomic_t first_cb;
  if (SDL_AtomicCAS(&first_cb, 0, 1))
    host_report_breadcrumb("first audio callback (len=%d)", len);
  if (!snesrecomp_sdl_lock_mutex(g_audio_mutex)) Die("Mutex lock failed!");
  ++g_audio_callback_count;
  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      uint32_t available = dsp_available(g_snes->apu->dsp);
      if (!g_audio_primed && available < HOST_AUDIO_PREFILL) {
        /* Startup/save-load starvation needs a cushion before playback
         * resumes. Retain all native PCM; count the undelivered output just
         * like any other underrun rather than hiding it from diagnostics. */
        memset(g_audiobuffer, 0, g_frames_per_block * g_audio_channels * sizeof(int16));
        audio_trace_on_output_underflow(available, g_frames_per_block);
      } else {
        g_audio_primed = true;
        RtlRenderAudio((int16 *)g_audiobuffer, g_frames_per_block, g_audio_channels);
        if (dsp_available(g_snes->apu->dsp) < 4)
          g_audio_primed = false;
      }
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end = g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16);
    }
    int n = IntMin(len, g_audiobuffer_end - g_audiobuffer_cur);
    if (g_sdl_audio_mixer_volume == SNESRECOMP_SDL_MIX_MAXVOLUME) {
      memcpy(stream, g_audiobuffer_cur, n);
    } else {
      SDL_memset(stream, 0, n);
#if SNESRECOMP_SDL3
      /* SDL3 takes a 0..1 float gain instead of a 0..128 integer volume. */
      SDL_MixAudio(stream, g_audiobuffer_cur, SDL_AUDIO_S16, n,
                   (float)g_sdl_audio_mixer_volume /
                       SNESRECOMP_SDL_MIX_MAXVOLUME);
#else
      SDL_MixAudioFormat(stream, g_audiobuffer_cur, AUDIO_S16, n,
                         g_sdl_audio_mixer_volume);
#endif
    }
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }
  SDL_UnlockMutex(g_audio_mutex);
}

#if SNESRECOMP_SDL3
static void SDLCALL AudioStreamCallback(
    void *userdata, SDL_AudioStream *stream, int additional_amount,
    int total_amount) {
  (void)userdata;
  (void)total_amount;
  if (additional_amount <= 0) return;
  if ((size_t)additional_amount > g_audio_stream_buffer_size) {
    uint8 *resized =
        (uint8 *)realloc(g_audio_stream_buffer, additional_amount);
    if (!resized) return;
    g_audio_stream_buffer = resized;
    g_audio_stream_buffer_size = (size_t)additional_amount;
  }
  FillAudioBuffer(g_audio_stream_buffer, additional_amount);
  SDL_PutAudioStreamData(stream, g_audio_stream_buffer, additional_amount);
}
#else
static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  FillAudioBuffer(stream, len);
}
#endif

static void SetAudioPaused(bool paused) {
#if SNESRECOMP_SDL3
  if (g_audio_stream) {
    if (paused) SDL_PauseAudioStreamDevice(g_audio_stream);
    else SDL_ResumeAudioStreamDevice(g_audio_stream);
  }
#else
  if (g_audio_device) SDL_PauseAudioDevice(g_audio_device, paused);
#endif
}

/* ── SDL_Renderer presenter ───────────────────────────────────────────────── */

static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_Rect g_sdl_renderer_rect;
static SDL_Rect g_sdl_present_rect;

static bool SdlRenderer_Init(SDL_Window *window) {
  (void)window;
  if (g_config.shader)
    fprintf(stderr, "Warning: Shaders are supported only with the OpenGL backend\n");

  /* SDL3 dropped the renderer flags argument (software vs accelerated is
   * chosen by driver name, vsync is set separately) and removed
   * SDL_RendererInfo entirely. snesrecomp_sdl_create_renderer() hides both. */
  bool want_software = g_config.output_method == kOutputMethod_SDLSoftware;
  SDL_Renderer *renderer = snesrecomp_sdl_create_renderer(
      g_window, want_software,
      /*vsync=*/!PresentationDecoupled() && !g_config.disable_frame_delay);
  if (renderer == NULL) {
    printf("Failed to create renderer: %s\n", SDL_GetError());
    return false;
  }
  if (kDebugFlag) {
    const char *name = snesrecomp_sdl_renderer_name(renderer);
    printf("Renderer: %s (vsync=%d)\n", name ? name : "(unknown)",
           snesrecomp_sdl_get_render_vsync(renderer));
  }
  g_renderer = renderer;

  g_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                g_snes_width, g_snes_height);
  if (g_texture == NULL) {
    printf("Failed to create texture: %s\n", SDL_GetError());
    return false;
  }
  /* SNES frames are opaque RGB with a zero alpha byte; SDL3 would blend
   * them away to the black clear colour. */
  snesrecomp_sdl_set_texture_opaque(g_texture);
  /* SDL3 sets filtering per-texture rather than through the global
   * SDL_HINT_RENDER_SCALE_QUALITY hint, so this must follow texture creation. */
  snesrecomp_sdl_set_texture_linear(g_texture, g_config.linear_filtering);
  return true;
}

static void SdlRenderer_Destroy(void) {
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
}

static void SdlRenderer_GetOutputSize(int *width, int *height) {
  if (!snesrecomp_sdl_get_render_output_size(g_renderer, width, height)) {
    *width = 0;
    *height = 0;
  }
}

static void SdlRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  /* SDL_QueryTexture is gone in SDL3; the shim reads w/h either way. */
  int texture_width = 0, texture_height = 0;
  snesrecomp_sdl_get_texture_size(g_texture, &texture_width, &texture_height);
  if (texture_width != width || texture_height != height) {
    SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!g_texture)
      Die("SDL texture allocation failed");
    snesrecomp_sdl_set_texture_linear(g_texture, g_config.linear_filtering);
  }
  snesrecomp_sdl_set_texture_opaque(g_texture);
  int output_width = 0, output_height = 0;
  SdlRenderer_GetOutputSize(&output_width, &output_height);
  SnesDisplayViewport viewport;
  SnesDisplayAspect_ComputeViewport(width, height, output_width, output_height,
                                    SnesDisplayAspect_Clamp(g_config.display_aspect),
                                    g_config.ignore_aspect_ratio, false, &viewport);
  g_sdl_present_rect.x = viewport.x;
  g_sdl_present_rect.y = viewport.y;
  g_sdl_present_rect.w = viewport.width;
  g_sdl_present_rect.h = viewport.height;
  g_sdl_renderer_rect.w = width;
  g_sdl_renderer_rect.h = height;
  if (!snesrecomp_sdl_lock_texture(g_texture, &g_sdl_renderer_rect,
                                   (void **)pixels, pitch)) {
    printf("Failed to lock texture: %s\n", SDL_GetError());
    *pixels = NULL;
    return;
  }
}

static void SdlRenderer_EndDraw(void) {
  SDL_UnlockTexture(g_texture);
  SDL_RenderClear(g_renderer);
  /* SDL3's SDL_RenderTexture takes SDL_FRect, not SDL_Rect. */
  snesrecomp_sdl_render_texture(g_renderer, g_texture, &g_sdl_renderer_rect,
                                &g_sdl_present_rect);
  SDL_RenderPresent(g_renderer);
}

static const struct RendererFuncs kSdlRendererFuncs = {
  &SdlRenderer_Init,
  &SdlRenderer_Destroy,
  &SdlRenderer_GetOutputSize,
  &SdlRenderer_BeginDraw,
  &SdlRenderer_EndDraw,
};

void MkDir(const char *s) {
#if defined(_WIN32)
  _mkdir(s);
#else
  mkdir(s, 0755);
#endif
}

/* ── Crash pipeline ───────────────────────────────────────────────────────── */

static void dump_cpu_state(void) {
  fprintf(stderr, "  CpuState: A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X PB=%02X "
                  "P=%02X m=%u x=%u e=%u\n",
                  g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.D, g_cpu.DB, g_cpu.PB,
                  g_cpu.P, g_cpu.m_flag, g_cpu.x_flag, g_cpu.emulation);
}
static void crash_handler(int sig) {
  extern const char *g_last_recomp_func;
  extern void RecompStackDump(void);
  fprintf(stderr, "\n*** CRASH (signal %d) in recomp func: %s ***\n",
          sig, g_last_recomp_func ? g_last_recomp_func : "(unknown)");
  dump_cpu_state();
  RecompStackDump();
  cpu_trace_dump_dbpb("CRASH — DB/PB mutations");
  cpu_trace_dump_recent("CRASH — main trace ring", 256);
  fflush(stderr);
  recomp_post_mortem_dump("signal", NULL);
  _exit(128 + sig);
}

#ifdef _WIN32
static LONG WINAPI seh_handler(EXCEPTION_POINTERS* info) {
  extern const char *g_last_recomp_func;
  extern void RecompStackDump(void);
  DWORD code = info->ExceptionRecord->ExceptionCode;
  void* addr = info->ExceptionRecord->ExceptionAddress;
  fprintf(stderr, "\n*** SEH CRASH code=0x%08lX at %p, last recomp func: %s ***\n",
          code, addr, g_last_recomp_func ? g_last_recomp_func : "(unknown)");
  if (code == EXCEPTION_ACCESS_VIOLATION) {
    ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
    ULONG_PTR fault_addr = info->ExceptionRecord->ExceptionInformation[1];
    fprintf(stderr, "    access violation: %s at 0x%p\n",
            kind == 0 ? "read" : (kind == 1 ? "write" : "execute"),
            (void*)fault_addr);
  }
  dump_cpu_state();
  RecompStackDump();
  cpu_trace_dump_dbpb("SEH CRASH — DB/PB mutations");
  cpu_trace_dump_recent("SEH CRASH — main trace ring", 256);
  fflush(stderr);
  recomp_post_mortem_dump("seh", info);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void post_mortem_atexit(void) {
  recomp_post_mortem_dump("atexit", NULL);
}

/* ── Mods / launcher / netplay glue ───────────────────────────────────────── */

#if SNESRECOMP_ENABLE_MODS
static int g_mods_ready;
#endif

#if defined(SNES_HAS_LOBBY_CLIENT)
static SnesNetplayConfig g_netplay_cfg;
static int g_netplay_pending;    /* launcher armed a session; start after SnesInit */
static int g_netplay_from_lobby; /* admit pump waits for the lobby peer */

static void host_lobby_ensure_init(void) {
  static int once;
  SnesHostLobbyIdentity id;
  SnesHostLobbyOpts opts;
  if (once)
    return;
  once = 1;
  memset(&id, 0, sizeof(id));
  id.game_name = g_game->display_name;
  id.game_version = g_game->build_version ? g_game->build_version : "dev";
  id.lan_registry_path = "netplay_lan_lobby.txt";
  id.default_lobby_name = "Netplay Lobby";
  memset(&opts, 0, sizeof(opts));
  opts.rematch_set_ready = 1;
  if (snes_host_lobby_init(&id, &opts) != 0)
    fprintf(stderr, "netplay: snes_host_lobby_init failed\n");
}

static uint16_t netplay_capture_pad(void *ctx) {
  (void)ctx;
  return (uint16_t)(OverlayNavInputs() & 0x0fffu);
}

static void netplay_poll_events(void *ctx, int *want_soft_exit) {
  SDL_Event event;
  (void)ctx;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT)
      *want_soft_exit = 2;
    if (event.type == SDL_KEYDOWN &&
        SNESRECOMP_SDL_EVENT_KEY(event) == SDLK_ESCAPE)
      *want_soft_exit = 1;
    if (event.type == SDL_KEYDOWN)
      HandleInput(SNESRECOMP_SDL_EVENT_KEY(event), SNESRECOMP_SDL_EVENT_MOD(event), true);
    if (event.type == SDL_KEYUP)
      HandleInput(SNESRECOMP_SDL_EVENT_KEY(event), SNESRECOMP_SDL_EVENT_MOD(event), false);
  }
}
#endif /* SNES_HAS_LOBBY_CLIENT */

/* The dump this project was generated from, if the player parked a copy next
 * to the executable or in the working directory. Not a requirement — it is
 * the habit tools/regen.sh documents, and skipping the prompt for someone
 * who already followed it is the whole point. */
static int FindRomBesideExe(char *out, size_t cap) {
  const char *name = g_game->rom_file;
  FILE *f;
  if (!name || !name[0])
    return 0;
  if (snesrecomp_exe_dir_path(name, out, cap)) {
    f = fopen(out, "rb");
    if (f) {
      fclose(f);
      return 1;
    }
  }
  f = fopen(name, "rb");
  if (f) {
    fclose(f);
    snprintf(out, cap, "%s", name);
    return 1;
  }
  out[0] = '\0';
  return 0;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int snesrecomp_desktop_main(const SnesDesktopHostGame *game, int argc, char **argv) {
  if (!game || !game->game_info || !game->display_name) {
    fprintf(stderr, "snesrecomp_desktop_main: descriptor needs display_name and game_info\n");
    return 2;
  }
  g_game = game;
  if (game->window_title && game->window_title[0])
    snprintf(g_window_title, sizeof(g_window_title), "%s", game->window_title);
  else
    snprintf(g_window_title, sizeof(g_window_title), "%s (Recompiled)", game->display_name);
  if (game->launcher_title && game->launcher_title[0])
    snprintf(g_launcher_title, sizeof(g_launcher_title), "%s", game->launcher_title);
  else
    snprintf(g_launcher_title, sizeof(g_launcher_title), "%s \xE2\x80\x94 Launcher", game->display_name);
  g_simulation_hz = game->simulation_hz > 0 ? game->simulation_hz : SNES_HOST_NTSC_HZ;
  g_snes_width = game->frame_width > 0 ? game->frame_width : 256;
  g_snes_height = game->frame_height > 0 ? game->frame_height : 224;
  const char *build_version = game->build_version ? game->build_version : "dev";

#ifndef _WIN32
  /* On Windows, do NOT install a SIGSEGV handler: the CRT's signal shim
   * intercepts access violations BEFORE SetUnhandledExceptionFilter, so
   * crashes would reach crash_handler with no EXCEPTION_POINTERS — no
   * exception record in the minidump/report. With SIGSEGV uninstalled,
   * AVs reach the SEH filter below with full fault context. */
  signal(SIGSEGV, crash_handler);
#endif
  signal(SIGABRT, crash_handler);
#ifdef __ANDROID__
  /* All relative I/O (config.ini, rom.cfg, saves/, last_run_report.json)
   * lands in the app's external files dir, which is adb-pushable. */
  {
    const char *storage = SDL_AndroidGetExternalStoragePath();
    if (storage) chdir(storage);
  }
#endif
#ifdef _WIN32
  SetUnhandledExceptionFilter(seh_handler);
  /* Suppress the Windows error dialog so SEH unwinds straight to our
   * filter and we can write the post-mortem report without the user
   * having to dismiss a popup first. */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  atexit(post_mortem_atexit);
  host_report_init(game->display_name, build_version);
  /* ARM the backwards watcher BEFORE any recompiled code runs. Without
   * this, the trace ring records but no tripwires fire. Heap-allocate the
   * cpu trace ring before any tripwire arms (override the size via
   * SNESRECOMP_CPU_TRACE_RING_ENTRIES). */
  cpu_trace_init();
  cpu_trace_arm_default_watches();
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  /* Capture program path before argv shift — used to place keybinds.ini
   * next to the executable. */
  const char *program_path = (argc >= 1) ? argv[0] : NULL;
  argc--, argv++;
  /* --launcher / --no-launcher may appear anywhere. A positional ROM used to
   * suppress the launcher outright, which was wrong in the one case that
   * matters most: Studio ALWAYS knows the ROM and always passes it, so the
   * launcher never appeared from its Build tab. A ROM on the command line
   * says which ROM to use, not whether a human is present. Suppression stays
   * explicit, because scripted harnesses launch as `<exe> <rom>` and expect
   * to boot straight in. */
  int force_launcher = 0, no_launcher = 0;
  {
    int w = 0;
    for (int i = 0; i < argc; ++i) {
      if (argv[i] && strcmp(argv[i], "--launcher") == 0) { force_launcher = 1; continue; }
      if (argv[i] && strcmp(argv[i], "--no-launcher") == 0) { no_launcher = 1; continue; }
      argv[w++] = argv[i];
    }
    argc = w;
  }
  const char *config_file = NULL;
  if (argc >= 2 && strcmp(argv[0], "--config") == 0) {
    config_file = argv[1];
    argc -= 2, argv += 2;
  } else {
    /* Anchor cwd to the binary's own directory FIRST. This is what makes an
     * AppImage work: host_paths.c prefers $APPIMAGE over /proc/self/exe, so
     * "next to the binary" means next to the user-visible .AppImage file
     * rather than inside the read-only squashfs mount. */
    int anchored = snesrecomp_anchor_to_exe_dir();
    if (!anchored) {
      /* Read-only install: fall back to the historical walk-up so a Windows
       * copy in an unwritable directory still finds a config. */
      SwitchDirectory();
    }
    EnsureConfigIniNextToExe(program_path);
    {
      char cwdbuf[1024];
      host_report_breadcrumb("config dir anchored: %s (exe-dir anchor: %s)",
                             getcwd(cwdbuf, sizeof(cwdbuf)) ? cwdbuf : "(unknown)",
                             anchored ? "ok" : "declined");
    }
  }
  int start_paused = 0;
  if (argc >= 1 && strcmp(argv[0], "--paused") == 0) {
    start_paused = 1;
    argc -= 1, argv += 1;
  }
  const char *script_file = NULL;
  if (argc >= 2 && strcmp(argv[0], "--script") == 0) {
    script_file = argv[1];
    argc -= 2, argv += 2;
  }
  const char *framedump_dir = NULL;
  if (argc >= 2 && strcmp(argv[0], "--framedump") == 0) {
    framedump_dir = argv[1];
    argc -= 2, argv += 2;
  }
  ParseConfigFile(config_file);
  g_active_config_file = config_file;
  /* Local overrides (gitignored). Last parser to set a key wins. */
  {
    FILE *f_local = fopen("config.local.ini", "rb");
    if (f_local) {
      fclose(f_local);
      ParseConfigFile("config.local.ini");
    }
  }
  if (game->after_config) game->after_config();
  host_report_breadcrumb(
      "config parsed: output=%d new_renderer=%d scale=%d fullscreen=%d "
      "audio=%d freq=%d samples=%d",
      g_config.output_method, g_config.new_renderer, g_config.window_scale,
      g_config.fullscreen, g_config.enable_audio, g_config.audio_freq,
      g_config.audio_samples);

#if SNESRECOMP_ENABLE_MODS
  /* Before the launcher, which needs the provider to show the Mods page.
   * The catalog is mods/preloaded beside the executable, staged by the build;
   * an empty one is valid. */
  if (game->game_id && game->game_id[0]) {
    char mods_dir[1024];
    if (snesrecomp_exe_dir_path("mods/preloaded", mods_dir, sizeof(mods_dir))) {
      g_mods_ready = snes_mod_runtime_initialize_c(
          mods_dir, game->game_id,
          game->expected_sha256_hex ? game->expected_sha256_hex : "");
      if (!g_mods_ready)
        fprintf(stderr, "mods: unavailable: %s\n", snes_mod_runtime_last_error_c());
    }
  }
#endif

  /* Resolve the SNES ROM path: launcher -> positional -> beside the exe ->
   * rom.cfg cache -> file picker. Every path checks the dump against the
   * digests the code was generated from. A 512-byte SMC copier header is
   * auto-stripped before hashing. */
  static char rom_path_buf[1024];
  {
    static uint8_t kExpectedSha256[32];
    static uint32_t kExpectedCrc32;
    static int rom_identity_ok;
    rom_identity_ok = DecodeRomIdentity(kExpectedSha256, &kExpectedCrc32);
    int rom_resolved_by_launcher = 0;
    char beside_exe[1024] = "";
    int have_positional = (argc >= 1 && argv[0] && argv[0][0] != '-' && argv[0][0] != '\0');
    if (!have_positional)
      FindRomBesideExe(beside_exe, sizeof(beside_exe));

#if defined(RECOMP_LAUNCHER)
    {
      /* A dummy video driver means CI or a screenshot harness: there is no
       * one to answer a GUI, and blocking on one would hang the job. */
      const char *vd = getenv("SDL_VIDEODRIVER");
      int headless = start_paused || (script_file != NULL) || (framedump_dir != NULL) ||
                     (vd && strcmp(vd, "dummy") == 0);
      const char *env_no_launcher = getenv("SNESRECOMP_NO_LAUNCHER");
      int want_launcher = !headless && !no_launcher && !(env_no_launcher && *env_no_launcher) &&
                          (force_launcher || !have_positional);

      /* SkipLauncher: boot straight from the cached ROM. A missing/unreadable
       * cache falls through to the launcher. */
      if (want_launcher && !force_launcher && g_config.skip_launcher) {
        char cached[1024]; cached[0] = '\0';
        if (snesrecomp_rom_cache_read(cached, sizeof(cached)) && cached[0]) {
          FILE *probe = fopen(cached, "rb");
          if (probe) {
            fclose(probe);
            snprintf(rom_path_buf, sizeof(rom_path_buf), "%s", cached);
            rom_resolved_by_launcher = 1;
            want_launcher = 0;
            host_report_breadcrumb("launcher skipped (SkipLauncher=1, cached rom)");
          }
        }
      }

      if (want_launcher) {
        host_report_breadcrumb("launcher: opening GUI");
        RecompLauncherCSettings ls;
        memset(&ls, 0, sizeof(ls));
        ls.output_method = g_config.output_method;
        ls.window_scale  = g_config.window_scale ? g_config.window_scale : 2;
        ls.fullscreen    = g_config.fullscreen;
        ls.ignore_aspect = g_config.ignore_aspect_ratio;
        ls.linear_filter = g_config.linear_filtering;
        ls.enable_audio  = g_config.enable_audio;
        ls.audio_freq    = g_config.audio_freq;
        ls.volume        = 100;
        ls.player_src[0] = g_config.enable_gamepad[0] ? 2 : 1;
        ls.player_src[1] = g_config.enable_gamepad[1] ? 2 : 0;
        /* Config stores deadzone as a raw stick radius; the launcher edits a
         * 0-100%. Convert in both directions. */
        ls.deadzone[0] = ls.deadzone[1] = g_config.gamepad_deadzone * 100 / 32767;
        ls.skip_launcher = g_config.skip_launcher;
        ls.msu1_enabled  = 0;

        /* Open on the ROM the player already has, so a second launch is PLAY
         * rather than Change-ROM: an explicit argument first, then the copy
         * beside the executable, then whatever the last run cached. */
        char init_rom[1024]; init_rom[0] = '\0';
        if (have_positional)
          snprintf(init_rom, sizeof(init_rom), "%s", argv[0]);
        else if (beside_exe[0])
          snprintf(init_rom, sizeof(init_rom), "%s", beside_exe);
        if (!init_rom[0] && !snesrecomp_rom_cache_read(init_rom, sizeof(init_rom)))
          init_rom[0] = '\0';

        RecompLauncherCGameInfo gi;
        memset(&gi, 0, sizeof(gi));
        /* SNES system identity (theme, platform label, ROM noun). One profile
         * call keeps the identity from drifting across SNES titles. */
        launcher_profile_apply("snes", &gi);
        static char region_buf[64];
        gi.name = game->display_name;
        if (game->region && game->region[0]) {
          snprintf(region_buf, sizeof(region_buf), "(%s)", game->region);
          gi.region = region_buf;
        }
        gi.sram_path = game->sram_path;
        gi.num_players = game->num_players > 0 ? game->num_players : 1;
        gi.expected_crc = kExpectedCrc32;
        gi.has_expected_crc = rom_identity_ok;
        gi.known_sha256 = rom_identity_ok
            ? (const uint8_t (*)[32])&kExpectedSha256 : NULL;
        gi.num_known_sha256 = rom_identity_ok ? 1 : 0;
        gi.widescreen_supported = game->widescreen_supported;
        gi.msu1_supported = game->msu1_supported;
        gi.config_path = config_file;  /* hotkey editor targets the live config */
        gi.mods = NULL;
        if (game->mods_provider)
          gi.mods = (const RecompLauncherCModProvider *)game->mods_provider();
#if SNESRECOMP_ENABLE_MODS
        if (!gi.mods && g_mods_ready)
          gi.mods = snes_mod_runtime_launcher_provider_c();
#endif
#if defined(SNES_HAS_LOBBY_CLIENT)
        /* The netplay button is capability-gated: these two fields are what
         * make the launcher show it. */
        gi.netplay_supported = 1;
        host_lobby_ensure_init();
        gi.netplay = snes_host_lobby_callbacks();
#endif
#if defined(SNESRECOMP_HOST_HAS_CODEGEN)
        /* Wire "Generate & rebuild…". No-ops when the SDK, CMake or the build
         * tree is absent, which is the normal state of a shipped build. */
        snesrecomp_codegen_host_autowire(&gi, gi.name);
#endif

        /* cwd is anchored to the exe dir and recomp_ui.cmake stages assets to
         * <exe>/assets, so "." resolves assets correctly. */
        int act = recomp_launcher_run_window(
            g_launcher_title, &ls, &gi, ".", init_rom[0] ? init_rom : NULL,
            rom_path_buf, sizeof(rom_path_buf));
        host_report_breadcrumb("launcher: action=%d rom=%s", act,
                               rom_path_buf[0] ? rom_path_buf : "(none)");
        if (act == RECOMP_LAUNCHER_RESULT_QUIT) {
          host_report_breadcrumb("exit: player quit from the launcher");
          return 0;
        }
#if defined(SNESRECOMP_HOST_HAS_CODEGEN)
        if (act == RECOMP_LAUNCHER_RESULT_RELAUNCH) {
          /* The player generated sources and rebuilt: this binary is stale.
           * Does not return on success. */
          snesrecomp_codegen_host_relaunch_or_exit(rom_path_buf);
          return 0;
        }
#endif
        if (act == RECOMP_LAUNCHER_RESULT_LAUNCH) {
          g_config.output_method       = (uint8)ls.output_method;
          g_config.window_scale        = (uint8)ls.window_scale;
          g_config.fullscreen          = (uint8)ls.fullscreen;
          g_config.ignore_aspect_ratio = ls.ignore_aspect != 0;
          g_config.linear_filtering    = ls.linear_filter != 0;
          g_config.enable_audio        = true;   /* always on */
          g_config.audio_freq          = (uint16)ls.audio_freq;
          g_config.enable_gamepad[0]   = ls.player_src[0] == 2;
          g_config.enable_gamepad[1]   = ls.player_src[1] == 2;
          g_config.gamepad_deadzone    = ls.deadzone[0] * 32767 / 100;
          g_config.skip_launcher       = ls.skip_launcher != 0;
          WriteConfigFile(config_file);
          /* The launcher's Hotkeys editor writes [KeyMap] straight into the
           * config file, which was parsed before the launcher ran — re-apply
           * so rebinds work on THIS boot, not the next one. */
          ConfigReloadKeyMap(config_file);
#if defined(SNES_HAS_LOBBY_CLIENT)
          /* A lobby launch arms the session; snes_netplay_start() runs after
           * SnesInit, once the guest exists. */
          if (ls.netplay_launch.enabled) {
            SnesHostLaunchResult res;
            snes_host_app_apply_launch(&ls.netplay_launch, &res);
            g_netplay_cfg = res.net_cfg;
            g_netplay_pending = 1;
            g_netplay_from_lobby = 1;
          }
#endif
          if (rom_path_buf[0]) {
            snesrecomp_rom_cache_write(rom_path_buf);
            rom_resolved_by_launcher = 1;
          }
        }
        /* UNAVAILABLE (assets or GL missing) -> console resolver below */
      }
    }
#else
    (void)force_launcher; (void)no_launcher;
#endif

    if (!rom_resolved_by_launcher) {
      char *la_argv[2] = {
        (char *)(program_path ? program_path : "snesrecomp"),
        (char *)(have_positional ? argv[0] : (beside_exe[0] ? beside_exe : ""))
      };
      int la_argc = (la_argv[1][0] != '\0') ? 2 : 1;
      if (!snesrecomp_launcher_resolve_rom_sha256(la_argc, la_argv, rom_path_buf,
                                                  sizeof(rom_path_buf),
                                                  rom_identity_ok ? kExpectedSha256
                                                                  : NULL)) {
        /* User cancelled the picker or repeatedly chose a non-matching ROM. */
        fprintf(stderr,
                "usage: %s [--config <ini>] [--paused] [--script <file>] "
                "[--framedump <dir>] [--launcher|--no-launcher] [path-to-rom.sfc]\n"
                "You must legally own a copy of %s.\n",
                program_path ? program_path : "<exe>", game->display_name);
        return 1;
      }
    }
  }
  static char *resolved_argv[2];
  resolved_argv[0] = rom_path_buf;
  resolved_argv[1] = NULL;
  argv = resolved_argv;
  argc = 1;
  host_report_breadcrumb("rom resolved: %s", rom_path_buf);

#if SNESRECOMP_ENABLE_MODS
  /* Resolve the enabled features against THIS ROM and persist the plan. A
   * rejected plan (wrong ROM for a package, a plugin nothing registered) is a
   * refusal, not a silent fallback. */
  if (g_mods_ready && !snes_mod_runtime_commit_c(rom_path_buf)) {
    fprintf(stderr, "mods: plan rejected: %s\n", snes_mod_runtime_last_error_c());
    return 1;
  }
#endif

  // Initialize debug server
  {
    /* Per-game debug server port so sibling games can run concurrently on
     * the same host without TCP-bind collisions. */
    int debug_port = game->debug_port > 0 ? game->debug_port : 4377;
    const char *debug_port_env = getenv("SNESRECOMP_DEBUG_PORT");
    if (debug_port_env && debug_port_env[0]) {
      char *end = NULL;
      long parsed = strtol(debug_port_env, &end, 0);
      if (end && *end == '\0' && parsed > 0 && parsed <= 65535) {
        debug_port = (int)parsed;
      } else {
        fprintf(stderr, "[main] Ignoring invalid SNESRECOMP_DEBUG_PORT='%s'\n",
                debug_port_env);
      }
    }
    if (debug_server_init(debug_port) == 0) {
#if SNESRECOMP_TRACE
      fprintf(stderr, "[main] Debug server ready on port %d\n", debug_port);
#endif
    } else {
      fprintf(stderr, "[main] Debug server failed to bind port %d\n", debug_port);
    }
    if (start_paused) {
      debug_server_start_paused();
#if SNESRECOMP_TRACE
      fprintf(stderr, "[main] Started paused — send 'step N' or 'continue' via TCP\n");
#endif
    }
  }

  g_gamepad[0].joystick_id = g_gamepad[1].joystick_id = -1;
  g_ws_extra = 0;
  g_ws_active = false;
  g_ppu_render_flags = g_config.new_renderer * kPpuRenderFlags_NewRenderer |
    g_config.no_sprite_limits * kPpuRenderFlags_NoSpriteLimits;

  if (g_config.fullscreen == 1)
    g_win_flags ^= SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP;
  else if (g_config.fullscreen == 2)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN;

  // Window scale (1=100%, 2=200%, 3=300%, etc.)
  g_current_window_scale = (g_config.window_scale == 0) ? 2 : IntMin(g_config.window_scale, kMaxWindowScale);

  // audio_freq: Use common sampling rates (values higher than 48000 are not supported.)
  if (g_config.audio_freq < 11025 || g_config.audio_freq > 48000)
    g_config.audio_freq = kDefaultFreq;

  // Currently, the SPC/DSP implementation only supports up to stereo.
  if (g_config.audio_channels < 1 || g_config.audio_channels > 2)
    g_config.audio_channels = kDefaultChannels;

  // audio_samples: power of 2
  if (g_config.audio_samples <= 0 || ((g_config.audio_samples & (g_config.audio_samples - 1)) != 0))
    g_config.audio_samples = kDefaultSamples;

  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

  // set up SDL
  SDL_SetMainReady();
  /* Return convention flipped in SDL3 (0 == success became true == success),
   * so this MUST go through the shim. */
  if (!snesrecomp_sdl_init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) {
    host_report_breadcrumb("SDL_Init FAILED: %s", SDL_GetError());
    printf("Failed to init SDL: %s\n", SDL_GetError());
    return 1;
  }
  host_report_breadcrumb("SDL init ok: video=%s audio=%s",
                         SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)",
                         SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");

  /* Load (or generate) keybinds.ini next to the executable. */
  keybinds_init(program_path);

  bool custom_size = g_config.window_width != 0 && g_config.window_height != 0;
  int window_width = custom_size ? g_config.window_width :
      g_current_window_scale * WindowBaseWidth(g_snes_width);
  int window_height = custom_size ? g_config.window_height :
      g_current_window_scale * WindowBaseHeight();

#ifndef __ANDROID__
  if (g_config.output_method == kOutputMethod_OpenGL) {
    g_win_flags |= SDL_WINDOW_OPENGL;
    OpenGLRenderer_Create(&g_renderer_funcs);
  } else
#endif
  {
    /* Android: always SDL_Renderer (GLES-backed); the desktop-GL presenter
     * is not compiled there. */
    g_renderer_funcs = kSdlRendererFuncs;
  }

  /* Load the SNES ROM. argv[0] is the resolved path. */
  uint8 *kRom = NULL;
  uint32 kRom_SIZE = 0;
  if (argv[0]) {
    size_t size;
    kRom = ReadWholeFile(argv[0], &size);
    kRom_SIZE = (uint32)size;
    if (!kRom)
      goto error_reading;
  }
  host_report_breadcrumb("rom loaded: %u bytes", kRom_SIZE);
  if (game->on_rom_loaded) game->on_rom_loaded(kRom, kRom_SIZE);

  RtlRegisterGame(game->game_info);
  Snes *snes = SnesInit(kRom, kRom_SIZE);
  host_report_breadcrumb("SnesInit: %s", snes ? "ok" : "FAILED");
  if (snes == NULL) {
error_reading:;
    char buf[256];
    snprintf(buf, sizeof(buf), "unable to load rom");
    Die(buf);
    return 1;
  }
#if SNESRECOMP_ENABLE_MODS
  /* Plugins act on a machine that exists: after SnesInit, before frame 1. */
  if (g_mods_ready)
    snes_mod_runtime_activate_plugins_c();
#endif
#if defined(SNES_HAS_LOBBY_CLIENT)
  if (g_netplay_pending) {
#if SNESRECOMP_ENABLE_MODS && defined(SNESRECOMP_NET_ROLLBACK)
    /* A mod that patches guest memory is simulation state. The host
     * publishes its effective set and every peer must confirm it before
     * the match may start; two peers on different sets cannot stay in
     * sync, so the netcode refuses with a reason instead of desyncing. */
    if (g_mods_ready) {
      static char s_modset[2048];
      int need = snes_mod_runtime_effective_set_c(s_modset, sizeof(s_modset));
      if (need >= 0 && need < (int)sizeof(s_modset))
        snes_netplay_rb_set_modset(s_modset, &snes_mod_runtime_check_set_c,
                                   &snes_mod_runtime_adopt_set_c);
      else
        fprintf(stderr, "mods: effective set is %d bytes, too large "
                "to publish -- netplay cannot gate on it\n", need);
    }
#endif
    int nrc = snes_netplay_start(&g_netplay_cfg);
    if (nrc != 0)
      fprintf(stderr, "netplay: snes_netplay_start failed (%d) — continuing offline\n", nrc);
    g_netplay_pending = 0;
  }
#endif

  // Connect debug server to SNES RAM
  debug_server_set_ram(snes->ram, 0x20000);

#ifdef ENABLE_ORACLE_BACKEND
  if (g_config.enable_snes9x_oracle) {
    extern int snes_oracle_init_default(const char *rom_path);
    int rc = snes_oracle_init_default(argv[0]);
    if (rc != 0)
      fprintf(stderr, "[oracle] init failed rc=%d (rom=%s)\n", rc, argv[0]);
    else
      fprintf(stderr, "[oracle] backend ready (rom=%s)\n", argv[0]);
  } else {
    extern void snes_oracle_set_disabled_by_game(const char *reason);
    static const char *kReason =
        "EnableSnes9xOracle is off in config.ini. The snes9x oracle starts "
        "from boot and cannot follow save-state loads, so a comparison that "
        "begins from a state diffs two unrelated moments.";
    snes_oracle_set_disabled_by_game(kReason);
  }
#endif

  /* SDL3 dropped the x/y arguments from SDL_CreateWindow. */
  SDL_Window *window = snesrecomp_sdl_create_window(
      g_window_title, window_width, window_height, g_win_flags);
  if(window == NULL) {
    host_report_breadcrumb("SDL_CreateWindow FAILED: %s", SDL_GetError());
    printf("Failed to create window: %s\n", SDL_GetError());
    return 1;
  }
  g_window = window;
  SDL_SetWindowHitTest(window, HitTestCallback, NULL);
  host_report_breadcrumb("window created: %dx%d flags=0x%x",
                         window_width, window_height, g_win_flags);

  if (!g_renderer_funcs.Initialize(window)) {
    host_report_breadcrumb("renderer init FAILED (output_method=%d)",
                           g_config.output_method);
    return 1;
  }
  host_report_breadcrumb("renderer initialized: %s",
      g_config.output_method == kOutputMethod_OpenGL ? "opengl" :
      g_config.output_method == kOutputMethod_SDLSoftware ? "sdl-software" : "sdl");

  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("No mutex");

  if (game->create_spc_player) {
    g_spc_player = game->create_spc_player();
    if (g_spc_player) {
      g_spc_player->initialize(g_spc_player);
      host_report_breadcrumb("SPC player initialized");
    }
  }

  int audio_output_rate = 0;
  if (g_config.enable_audio) {
    /* Enumerate output devices into the breadcrumb ring: which device
     * SDL picks (and what else was available) is exactly the per-machine
     * variable a non-reproducible audio/boot crash report needs. */
    {
#if SNESRECOMP_SDL3
      int ndev = 0;
      SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&ndev);
      host_report_breadcrumb("audio outputs: %d device(s)", ndev);
      for (int i = 0; i < ndev && i < 8; i++)
        host_report_breadcrumb("audio output[%d]: %s", i,
                               SDL_GetAudioDeviceName(devices[i]));
      SDL_free(devices);
#else
      int ndev = SDL_GetNumAudioDevices(0);
      host_report_breadcrumb("audio outputs: %d device(s)", ndev);
      for (int i = 0; i < ndev && i < 8; i++)
        host_report_breadcrumb("audio output[%d]: %s", i,
                               SDL_GetAudioDeviceName(i, 0));
#endif
    }
    SDL_AudioSpec want = { 0 }, have;
    want.freq = g_config.audio_freq;
    want.format = AUDIO_S16;
    want.channels = 2;
#if SNESRECOMP_SDL3
    /* SDL3 has no `samples`/`callback` in SDL_AudioSpec: the device is opened
     * as a stream and the callback is supplied separately. */
    have = want;
    g_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, AudioStreamCallback, NULL);
    if (g_audio_stream) {
      g_audio_device = SDL_GetAudioStreamDevice(g_audio_stream);
      SDL_GetAudioStreamFormat(g_audio_stream, &have, NULL);
    }
#else
    want.samples = g_config.audio_samples;
    want.callback = &AudioCallback;
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
#endif
    if (g_audio_device == 0) {
      host_report_breadcrumb("audio device open FAILED: %s", SDL_GetError());
      printf("Failed to open audio device: %s\n", SDL_GetError());
      return 1;
    }
    g_audio_channels = 2;
    /* The SPC's native rate is 32040 Hz (1.024 MHz / 32), not 32000. The
     * consumer converts onto the device rate and cannot infer it. */
    RtlSetAudioOutputRate(have.freq);
    audio_output_rate = have.freq;
    g_frames_per_block = (534 * have.freq + 32040 / 2) / 32040;
    g_audiobuffer = (uint8 *)calloc(g_frames_per_block * have.channels * sizeof(int16), 1);
    host_report_breadcrumb(
        "audio device opened: freq=%d (want %d) ch=%d samples=%d frames_per_block=%d",
        have.freq, want.freq, have.channels,
#if SNESRECOMP_SDL3
        g_config.audio_samples,
#else
        have.samples,
#endif
        g_frames_per_block);
  } else {
    host_report_breadcrumb("audio disabled in config");
  }

  PreparePpuFrame();

  MkDir("saves");
  RtlReadSram();

  {
#if SNESRECOMP_SDL3
    int njs = 0;
    SDL_JoystickID *joysticks = SDL_GetJoysticks(&njs);
#else
    int njs = SDL_NumJoysticks();
#endif
    printf("[Gamepad] SDL reports %d joystick(s) at startup. "
           "enable_gamepad=[%d,%d]\n",
           njs, g_config.enable_gamepad[0], g_config.enable_gamepad[1]);
    for (int i = 0; i < njs; i++) {
#if SNESRECOMP_SDL3
      /* SDL3 enumerates by instance ID rather than by index. */
      SDL_JoystickID joystick = joysticks[i];
      const char *name = SDL_GetJoystickNameForID(joystick);
      int is_gc = SDL_IsGamepad(joystick);
#else
      SDL_JoystickID joystick = i;
      const char *name = SDL_JoystickNameForIndex(i);
      int is_gc = SDL_IsGameController(i);
#endif
      printf("[Gamepad]   #%d name=%s is_game_controller=%d\n",
             i, name ? name : "(null)", is_gc);
      OpenOneGamepad(joystick);
    }
#if SNESRECOMP_SDL3
    SDL_free(joysticks);
#endif
    if (njs == 0) {
      printf("[Gamepad] No joysticks detected. "
             "On Windows, plug controller in BEFORE launching, "
             "or check that XInput drivers are installed.\n");
    }
  }

  if (g_config.autosave)
    HandleCommand(kKeys_Load + 0, true);

  if (script_file)
    LoadScript(script_file);

  if (framedump_dir)
    FrameDump_Init(framedump_dir);

  RtlEnableExtendedFrameTiming();
  bool running = true;
  const char *exit_reason = "loop ended";
  uint32 frameCtr = 0;
  const char *run_frames_env = HostGetenv("RUN_FRAMES");
  unsigned run_frames = run_frames_env ? (unsigned)strtoul(run_frames_env, NULL, 10) : 0;
  const char *trace_path = HostGetenv("STATE_TRACE");
  FILE *state_trace = trace_path ? fopen(trace_path, "w") : NULL;
  uint64_t presentations = 0;
  bool profile_requested = HostGetenv("HOST_PROFILE") && atoi(HostGetenv("HOST_PROFILE")) != 0;
  unsigned profile_first = HostGetenv("HOST_PROFILE_START_FRAME")
      ? (unsigned)strtoul(HostGetenv("HOST_PROFILE_START_FRAME"), NULL, 10) : 1;
  if (!profile_first) profile_first = 1;
  double profile_window_start = 0;
  double run_start = MonotonicSeconds();
  uint8 audiopaused = true;
  GamepadInfo *gi;
  SnesHostClock video_clock;
  double presentation_hz = WantedPresentationHz(DisplayRefresh());
  if (presentation_hz <= 0) presentation_hz = g_simulation_hz;
  snes_host_clock_reset(&video_clock, MonotonicSeconds(), g_simulation_hz, presentation_hz);
  double next_display_check = 0;

  /* Rewind ring: reads the env overrides and reserves slot headers; the
   * buffer itself is allocated lazily on the first capture. */
  snes_rewind_configure();

  host_report_breadcrumb("entering main loop");

  while (running) {
    SDL_Event event;

    /* Inert unless SNESRECOMP_CRASH_TEST is set — support drill for the
     * whole crash-capture pipeline (minidump + report + crash copy). */
    host_report_crash_test_tick();

    double event_profile_start = ProfileStart();
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_CONTROLLERDEVICEADDED:
        OpenOneGamepad(event.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        gi = GetGamepadInfo(SNESRECOMP_SDL_EVENT_DEVICE(event));
        if (gi) {
          memset(gi, 0, sizeof(GamepadInfo));
          gi->joystick_id = -1;
        }
        break;
      case SDL_CONTROLLERAXISMOTION:
        gi = GetGamepadInfo(SNESRECOMP_SDL_EVENT_AXIS_DEVICE(event));
        if (gi)
          HandleGamepadAxisInput(gi, SNESRECOMP_SDL_EVENT_AXIS(event),
                                 SNESRECOMP_SDL_EVENT_AXIS_VALUE(event));
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        gi = GetGamepadInfo(SNESRECOMP_SDL_EVENT_BUTTON_DEVICE(event));
        if (gi) {
          int b = RemapSdlButton(SNESRECOMP_SDL_EVENT_BUTTON(event));
          if (b >= 0)
            HandleGamepadInput(gi, b, event.type == SDL_CONTROLLERBUTTONDOWN);
        }
        break;
      }
      /* Unmapped joysticks (no SDL_GameController mapping): the raw Steam
       * virtual gamepad layout is the standard Xbox button order. */
      case SDL_JOYDEVICEADDED:
        OpenOneJoystick(event.jdevice.which);
        break;
      case SDL_JOYDEVICEREMOVED:
        gi = GetGamepadInfo(event.jdevice.which);
        if (gi && gi->raw_joystick) {
          if (gi->joystick) SDL_JoystickClose(gi->joystick);
          memset(gi, 0, sizeof(GamepadInfo));
          gi->joystick_id = -1;
        }
        break;
      case SDL_JOYAXISMOTION:
        gi = GetGamepadInfo(event.jaxis.which);
        if (gi && gi->raw_joystick)
          HandleGamepadAxisInput(gi, event.jaxis.axis, event.jaxis.value);
        break;
      case SDL_JOYBUTTONDOWN:
      case SDL_JOYBUTTONUP:
        gi = GetGamepadInfo(event.jbutton.which);
        if (gi && gi->raw_joystick && event.jbutton.button < 15) {
          static const uint8 raw_buttons[] = {
            kGamepadBtn_A, kGamepadBtn_B, kGamepadBtn_X, kGamepadBtn_Y,
            kGamepadBtn_Back, kGamepadBtn_Guide, kGamepadBtn_Start,
            kGamepadBtn_L3, kGamepadBtn_R3, kGamepadBtn_L1, kGamepadBtn_R1,
            kGamepadBtn_DpadUp, kGamepadBtn_DpadDown,
            kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight
          };
          HandleGamepadInput(gi, raw_buttons[event.jbutton.button],
                             event.type == SDL_JOYBUTTONDOWN);
        }
        break;
      case SDL_MOUSEWHEEL:
        if (SDL_GetModState() & KMOD_CTRL && event.wheel.y != 0)
          ChangeWindowScale(event.wheel.y > 0 ? 1 : -1);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2) {
          if ((g_win_flags & SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 && (g_win_flags & SDL_WINDOW_FULLSCREEN) == 0 && SDL_GetModState() & KMOD_SHIFT) {
            g_win_flags ^= SDL_WINDOW_BORDERLESS;
            SDL_SetWindowBordered(g_window, (g_win_flags & SDL_WINDOW_BORDERLESS) == 0 ? SDL_TRUE : SDL_FALSE);
          }
        }
        break;
      case SDL_KEYDOWN:
        HandleInput(SNESRECOMP_SDL_EVENT_KEY(event),
                    SNESRECOMP_SDL_EVENT_MOD(event), true);
        break;
      case SDL_KEYUP:
        HandleInput(SNESRECOMP_SDL_EVENT_KEY(event),
                    SNESRECOMP_SDL_EVENT_MOD(event), false);
        break;
      case SDL_QUIT:
        running = false;
        exit_reason = "SDL_QUIT event";
        break;
      }
    }
    if (!running)
      break;

    ProfileEnd(kProfileEvents, event_profile_start);
    if (g_paused != audiopaused) {
      audiopaused = g_paused;
      SetAudioPaused(audiopaused);
    }

    if (g_paused) {
      snes_host_clock_reset(&video_clock, MonotonicSeconds(), g_simulation_hz, presentation_hz);
      SDL_Delay(16);
      continue;
    }

    // Clear gamepad inputs when joypad directional inputs to avoid wonkiness
    if (g_input_state & 0xf0)
      g_gamepad[0].axis_buttons = 0;
    if (g_input_state & 0xf0000)
      g_gamepad[1].axis_buttons = 0;
    {
      int ls = debug_server_consume_loadstate();
      if (ls >= 0) {
        RtlSaveLoad(kSaveLoad_Load, ls);
        GameReset();
      }
      int ss = debug_server_consume_savestate();
      if (ss >= 0)
        RtlSaveLoad(kSaveLoad_Save, ss);
    }
    double before_debug_wait = MonotonicSeconds();
    debug_server_wait_if_paused();
    if (MonotonicSeconds() - before_debug_wait > 0.05)
      g_reset_clock = true;

#if defined(SNES_HAS_LOBBY_CLIENT)
    /* Netplay session: the delay-sync admit pump owns the frame cadence.
     * Both seats' inputs come back merged from the netcode; on a stall the
     * held framebuffer is re-presented so the window stays live. */
    if (snes_netplay_active()) {
      SnesHostBarrierHooks hooks;
      int run = running;
      int admitted;
      memset(&hooks, 0, sizeof(hooks));
      hooks.capture_local_pad = &netplay_capture_pad;
      hooks.poll_events = &netplay_poll_events;
      admitted = snes_host_barrier_admit(g_netplay_from_lobby, &run, &hooks);
      running = run;
      if (!running) {
        exit_reason = "netplay barrier requested exit";
        break;
      }
      if (admitted) {
        int burst = 0;
        for (;;) {
          uint32 inputs = snes_netplay_published_inputs() | snes_netplay_active_mask();
          if (game->before_run_frame) game->before_run_frame();
          RtlRunFrame(inputs);
          frameCtr++;
          g_screenshot_frame = frameCtr;
          snes_osd_note_frame();
          snes_rewind_note_frame();
          CaptureSimulationFrame(frameCtr);
          snes_netplay_finish_frame();
          if (burst >= snes_host_catchup_budget())
            break;
          snes_netplay_stage_local(netplay_capture_pad(NULL));
          if (!snes_netplay_poll_admit())
            break;
          burst++;
        }
      }
      g_present_alpha = 1;
      DrawPpuFrameWithPerf();
      ++presentations;
      snes_host_clock_reset(&video_clock, MonotonicSeconds(), g_simulation_hz, presentation_hz);
      continue;
    }
#endif /* SNES_HAS_LOBBY_CLIENT */

    /* Pacing. A title that decouples presentation (presentation_hz hook)
     * gets re-presented between simulated frames from the captured field;
     * everyone else presents once per simulated frame. */
    bool paced_realtime = !g_turbo && !g_config.disable_frame_delay;
    bool paced_custom = PresentationDecoupled() && paced_realtime;
    double video_now = MonotonicSeconds();
    if (video_now >= next_display_check) {
      double hz = WantedPresentationHz(DisplayRefresh());
      if (hz <= 0) hz = g_simulation_hz;
      if (hz != presentation_hz) {
        presentation_hz = video_clock.presentation_hz = hz;
        video_clock.next_presentation = video_now;
      }
      next_display_check = video_now + 0.25;
    }
    if (g_reset_clock) {
      snes_host_clock_reset(&video_clock, video_now, g_simulation_hz, presentation_hz);
      g_reset_clock = false;
    }
    if (paced_realtime && !snes_host_clock_simulation_due(&video_clock, MonotonicSeconds())) {
      if (snes_host_clock_presentation_due(&video_clock, MonotonicSeconds())) {
        if (paced_custom) {
          double presented_at = MonotonicSeconds();
          g_present_alpha = presentation_hz != g_simulation_hz
              ? snes_host_clock_alpha(&video_clock, presented_at) : 1;
          DrawPpuFrameWithPerf();
          ++presentations;
          /* Count deadlines at dispatch, not after the render/upload cost:
           * crossing the next deadline while drawing does not consume it. */
          snes_host_clock_presentation_done(&video_clock, presented_at);
        }
      }
      WaitUntil(paced_custom ? snes_host_clock_next_deadline(&video_clock)
                             : video_clock.next_simulation);
      continue;
    }

    /* Drive the SNES controller bits in g_input_state from keybinds.ini.
     * config.ini's [KeyMap] still owns system commands (state save/load,
     * fullscreen, pause, etc.); the 12 controller buttons per player come
     * from keybinds.ini. Mapping: keybinds bit layout (see keybinds.h) ->
     * kKeys_Controls index ([Controls] order: Up Down Left Right Select
     * Start A B X Y L R). HandleCommand is idempotent for set/clear. */
    {
      const uint8_t *keys = snesrecomp_sdl_get_keyboard_state();
      uint16_t kb_p1 = keybinds_read_player(keys, 1);
      uint16_t kb_p2 = keybinds_read_player(keys, 2);
      static const uint8 kKb2CtrlsIdx[12] = { 7, 6, 5, 4, 9, 8, 3, 11, 2, 10, 1, 0 };
      for (int i = 0; i < 12; i++) {
        HandleCommand(kKeys_Controls   + i, (kb_p1 >> kKb2CtrlsIdx[i]) & 1);
        HandleCommand(kKeys_ControlsP2 + i, (kb_p2 >> kKb2CtrlsIdx[i]) & 1);
      }
    }

    /* Seat 0's HUMAN word, kept separate from the script's and the debug
     * server's: the overlays are human facilities, and a repro script must
     * never be able to open a modal panel it has no way to close.
     *
     * Filtered ONCE per frame, and the filtered word is what both the guest
     * and the open gesture see -- which is what snes_savestate_menu.h asks
     * for. Passing the unfiltered word to the gesture means the button still
     * held when the browser closed re-satisfies Select+R on the very next
     * frame, so it reopens immediately, every frame, and the game never
     * advances again. A resting analog stick or a held shoulder button is
     * enough to trigger it. */
    {
      static long load_frame = -2;
      if (load_frame == -2) {
        const char *v = HostGetenv("OVERLAY_SELFTEST_LOADAT");
        load_frame = v ? strtol(v, NULL, 0) : -1;
      }
      if (load_frame >= 0 && (long)frameCtr == load_frame) {
        fprintf(stderr, "[overlay_selftest] loading the state saved earlier, at frame %ld\n",
                load_frame);
        (void)snes_savestate_menu_poll_open(SNES_PAD_SELECT | SNES_PAD_R);
        if (snes_savestate_menu_is_open()) {
          uint32_t t = SDL_GetTicks();
          snes_savestate_menu_poll_nav(SNES_PAD_A, t);     /* A = load */
          snes_savestate_menu_poll_nav(0, t + 1);
          snes_savestate_menu_close();
          GameReset();
          fprintf(stderr, "[overlay_selftest] load issued\n");
        }
      }
    }

    uint32 human = snes_savestate_menu_filter_guest_input(OverlayNavInputs());
    uint32 inputs = human | (g_gamepad[1].axis_buttons << 12);
    inputs |= TickScript();
    inputs |= debug_server_get_controller_inputs();

    /* Overlay self-test (SNESRECOMP_OVERLAY_SELFTEST=<frame>, off by default).
     * The overlays can only be driven by a human, so nothing automated ever
     * exercised the modal path -- and what shipped there froze the game.
     * This opens the browser at a chosen frame, pumps the present path the
     * modal loop uses, closes it, and lets the run continue. The property
     * being checked is exact: a frozen overlay must leave the guest BIT
     * IDENTICAL, so a traced run with this armed must match one without it. */
    {
      static long selftest_frame = -2;
      if (selftest_frame == -2) {
        const char *v = HostGetenv("OVERLAY_SELFTEST");
        selftest_frame = v ? strtol(v, NULL, 0) : -1;
      }
      if (selftest_frame >= 0 && (long)frameCtr == selftest_frame) {
        fprintf(stderr, "[overlay_selftest] opening save-state browser at frame %ld\n",
                selftest_frame);
        (void)snes_savestate_menu_poll_open(SNES_PAD_SELECT | SNES_PAD_R);
        if (!snes_savestate_menu_is_open()) {
          fprintf(stderr, "[overlay_selftest] FAILED: gesture did not open it\n");
        } else {
          for (int i = 0; i < 30; i++)
            PresentFrozenWithOverlay();
          /* Save/load are pad-only (X saves, A loads), so this synthesizes
           * those edges. */
          if (HostGetenv("OVERLAY_SELFTEST_SAVEONLY")) {
            uint32_t t = SDL_GetTicks();
            fprintf(stderr, "[overlay_selftest] pad X (save) only\n");
            snes_savestate_menu_poll_nav(SNES_PAD_X, t);
            snes_savestate_menu_poll_nav(0, t + 1);
          }
          if (HostGetenv("OVERLAY_SELFTEST_SAVELOAD")) {
            uint32_t t = SDL_GetTicks();
            fprintf(stderr, "[overlay_selftest] pad X (save)...\n");
            snes_savestate_menu_poll_nav(SNES_PAD_X, t);
            snes_savestate_menu_poll_nav(0, t + 1);
            PresentFrozenWithOverlay();
            fprintf(stderr, "[overlay_selftest] pad A (load)...\n");
            snes_savestate_menu_poll_nav(SNES_PAD_A, t + 2);
            snes_savestate_menu_poll_nav(0, t + 3);
            fprintf(stderr, "[overlay_selftest] after load, menu is %s\n",
                    snes_savestate_menu_is_open() ? "open" : "closed");
          }
          snes_savestate_menu_close();
          fprintf(stderr, "[overlay_selftest] pumped 30 present passes, closed: %s\n",
                  snes_savestate_menu_is_open() ? "STILL OPEN" : "ok");
          GameReset();
        }
      }
    }

    if (g_rewind_hotkey && !snes_rewind_is_open() &&
        !snes_savestate_menu_is_open()) {
      /* Refused during netplay by snes_rewind_open() itself: one machine
       * cannot move its own clock backwards while a peer is watching. */
      if (snes_rewind_open()) {
        RunRewindLoop(&running);
        continue;   /* guest was frozen: no frame to run or present */
      }
    }
    g_rewind_hotkey = 0;
    /* Exactly ONE poll_open per frame: it latches the previous word to edge
     * detect on, so a second call in the same frame eats the edge. */
    (void)snes_savestate_menu_poll_open(human);
    if (g_savestate_menu_hotkey) {
      g_savestate_menu_hotkey = 0;
      if (!snes_savestate_menu_is_open())
        (void)snes_savestate_menu_poll_open(SNES_PAD_SELECT | SNES_PAD_R);
    }
    if (snes_savestate_menu_is_open()) {
      RunSavestateMenuLoop(&running);
      GameReset();
      continue;   /* guest was frozen: no frame to run or present */
    }
    g_profile_frame = frameCtr + 1;
    if (profile_requested && !g_profile && g_profile_frame >= profile_first) {
      g_profile = true;
      profile_window_start = MonotonicSeconds();
    }
    double profile_start = ProfileStart();
    double guest_start = MonotonicSeconds();
    if (game->before_run_frame) game->before_run_frame();
    g_audio_producer_active = paced_realtime && g_audio_device != 0;
    RtlRunFrame(inputs | GetActiveControllers() | debug_server_get_controller_active_mask());
    ApplyScriptForcePokes();
    snes_osd_note_frame();
    /* One guest frame happened: offer it to the rewind ring, and offer the
     * field as the next save's thumbnail. Both are no-ops while a panel is
     * open, so a thumbnail is of the game and not of the overlay. */
    snes_rewind_note_frame();
    if (g_ppu && g_ppu->renderBuffer) {
      snes_savestate_menu_note_frame((const uint32_t *)g_ppu->renderBuffer,
                                     256, g_snes_height);
      snes_rewind_note_framebuffer((const uint32_t *)g_ppu->renderBuffer,
                                   256, g_snes_height);
    }
    ProfileEnd(kProfileGuest, profile_start);
    frameCtr++;
    g_screenshot_frame = frameCtr;
    if (game->after_run_frame) {
      profile_start = ProfileStart();
      double now = MonotonicSeconds();
      SnesDesktopHostFrameStats st = {
        .frame = frameCtr,
        .run_seconds = now - run_start,
        .guest_seconds = now - guest_start,
        .audio_output_rate = audio_output_rate,
      };
      game->after_run_frame(&st);
      ProfileEnd(kProfileGameHook, profile_start);
    }

#ifdef ENABLE_ORACLE_BACKEND
    {
      extern void emu_oracle_run_frame(uint16_t j1, uint16_t j2);
      emu_oracle_run_frame(runner_to_snes_joypad((uint16_t)(inputs & 0xFFF)),
                           runner_to_snes_joypad((uint16_t)((inputs >> 12) & 0xFFF)));
    }
#endif

    if (frameCtr == 1)
      host_report_breadcrumb("first frame simulated");
    else if (frameCtr % 3600 == 0)   /* ~once a minute at 60 fps */
      host_report_breadcrumb("heartbeat: frame=%u", frameCtr);
    g_snes->disableRender = g_turbo && (frameCtr & 0xf) != 0;
    snes_osd_set_turbo(g_turbo);

    profile_start = ProfileStart();
    CaptureSimulationFrame(frameCtr);
    g_audio_producer_active = false;
    ProfileEnd(kProfileRaster, profile_start);
    profile_start = ProfileStart();
    if (state_trace)
      fprintf(state_trace, "%u,%08x,%04x,%04x,%04x,%04x,%04x,%02x,%02x,%02x\n",
              frameCtr, crc32_compute(g_ram, 0x20000), g_cpu.A, g_cpu.X, g_cpu.Y,
              g_cpu.S, g_cpu.D, g_cpu.DB, g_cpu.PB, g_cpu.P);
    ProfileEnd(kProfileTrace, profile_start);
    if (paced_realtime) {
      bool keep_debt = game->keep_pacing_debt && game->keep_pacing_debt();
      snes_host_clock_simulation_done(&video_clock, MonotonicSeconds(), keep_debt,
                                      RtlLastFramePeriods());
    }
    else
      snes_host_clock_reset(&video_clock, MonotonicSeconds(), g_simulation_hz, presentation_hz);
    if (!g_snes->disableRender &&
        (!paced_custom || snes_host_clock_presentation_due(&video_clock, MonotonicSeconds()) ||
         (run_frames && frameCtr >= run_frames))) {
      double presented_at = MonotonicSeconds();
      g_present_alpha = paced_custom && presentation_hz != g_simulation_hz
          ? snes_host_clock_alpha(&video_clock, presented_at) : 1;
      DrawPpuFrameWithPerf();
      ++presentations;
      snes_host_clock_presentation_done(&video_clock, presented_at);
    }
    if (run_frames && frameCtr >= run_frames) {
      running = false;
      exit_reason = "RUN_FRAMES reached";
    }
  }

  if (state_trace) fclose(state_trace);
  host_report_breadcrumb("exit: %s after %u frames", exit_reason, frameCtr);
  host_report_breadcrumb("video totals: simulations=%u presentations=%llu seconds=%.3f",
                         frameCtr, (unsigned long long)presentations,
                         MonotonicSeconds() - run_start);
  if (g_profile) {
    double profile_seconds = MonotonicSeconds() - profile_window_start;
    host_report_breadcrumb("video profile window: first=%u last=%u seconds=%.6f presentations=%u",
        profile_first, frameCtr, profile_seconds, g_timings[kProfilePresent].count);
    static const char *names[kProfileCount] = {
      "guest", "raster-capture", "surface-acquire", "compose", "upload-present", "state-trace", "game-hook",
      "event-pump", "deadline-wait"
    };
    for (unsigned i = 0; i < kProfileCount; ++i)
      host_report_breadcrumb("video profile: stage=%s count=%u total_ms=%.3f mean_ms=%.3f max_ms=%.3f max_frame=%u",
          names[i], g_timings[i].count, g_timings[i].total * 1000,
          g_timings[i].count ? g_timings[i].total * 1000 / g_timings[i].count : 0,
          g_timings[i].maximum * 1000, g_timings[i].maximum_frame);
  }

  if (g_config.autosave)
    HandleCommand(kKeys_Save + 0, true);

  RtlWriteSram();

  // clean sdl
  SetAudioPaused(true);
#if SNESRECOMP_SDL3
  /* Destroying the stream closes the device it was opened against. */
  SDL_DestroyAudioStream(g_audio_stream);
  g_audio_stream = NULL;
#else
  SDL_CloseAudioDevice(g_audio_device);
#endif
  SDL_DestroyMutex(g_audio_mutex);
  free(g_audiobuffer);

  g_renderer_funcs.Destroy();

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

/* ── Input plumbing ───────────────────────────────────────────────────────── */

static void RenderDigit(uint8 *dst, size_t pitch, int digit, uint32 color, bool big) {
  static const uint8 kFont[] = {
    0x1c, 0x36, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x36, 0x1c,
    0x18, 0x1c, 0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e,
    0x3e, 0x63, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x63, 0x7f,
    0x3e, 0x63, 0x60, 0x60, 0x3c, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x30, 0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x30, 0x30, 0x78,
    0x7f, 0x03, 0x03, 0x03, 0x3f, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x1c, 0x06, 0x03, 0x03, 0x3f, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x7f, 0x63, 0x60, 0x60, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x0c,
    0x3e, 0x63, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x3e, 0x63, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x60, 0x30, 0x1e,
  };
  const uint8 *p = kFont + digit * 10;
  if (!big) {
    for (int y = 0; y < 10; y++, dst += pitch) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1)
          ((uint32 *)dst)[x] = color;
      }
    }
  } else {
    for (int y = 0; y < 10; y++, dst += pitch * 2) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1) {
          ((uint32 *)dst)[x * 2 + 1] = ((uint32 *)dst)[x * 2] = color;
          ((uint32 *)(dst + pitch))[x * 2 + 1] = ((uint32 *)(dst + pitch))[x * 2] = color;
        }
      }
    }
  }
}

static void RenderNumber(uint8 *dst, size_t pitch, int n, uint8 big) {
  char buf[32], *s;
  int i;
  sprintf(buf, "%d", n);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + ((pitch + i + 4) << big), pitch, *s - '0', 0x404040, big);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + (i << big), pitch, *s - '0', 0xffffff, big);
}

static void HandleCommand(uint32 j, bool pressed) {
  static const uint8 kKbdRemap[] = { 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
  if (j < kKeys_Controls)
    return;

  if (j <= kKeys_Controls_Last) {
    uint32 m = 1 << kKbdRemap[j - kKeys_Controls];
    g_input_state = pressed ? (g_input_state | m) : (g_input_state & ~m);
    return;
  }

  if (j <= kKeys_ControlsP2_Last) {
    uint32 m = 0x1000 << kKbdRemap[j - kKeys_ControlsP2];
    g_input_state = pressed ? (g_input_state | m) : (g_input_state & ~m);
    return;
  }

  if (j == kKeys_Turbo) {
    g_turbo = pressed;
    return;
  }

  if (!pressed)
    return;
  if (j <= kKeys_Load_Last) {
    RtlSaveLoad(kSaveLoad_Load, j - kKeys_Load);
    GameReset();
  } else if (j <= kKeys_Save_Last) {
    RtlSaveLoad(kSaveLoad_Save, j - kKeys_Save);
  } else {
    switch (j) {
    case kKeys_Fullscreen:
      g_win_flags ^= SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP;
      SDL_SetWindowFullscreen(g_window, g_win_flags & SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP);
      g_cursor = !g_cursor;
      snesrecomp_sdl_show_cursor(g_cursor);
      break;
    case kKeys_Reset:
      RtlReset(1);
      GameReset();
      break;
    case kKeys_Pause: g_paused = !g_paused; break;
    case kKeys_PauseDimmed:
      g_paused = !g_paused;
#ifdef _WIN32
      if (g_paused && g_renderer) {
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 159);
        SDL_RenderFillRect(g_renderer, NULL);
        SDL_RenderPresent(g_renderer);
      }
#endif
      break;
    case kKeys_WindowBigger: ChangeWindowScale(1); break;
    case kKeys_WindowSmaller: ChangeWindowScale(-1); break;
    case kKeys_DisplayPerf:
      g_display_perf ^= 1;
      snes_osd_set_fps_visible(g_display_perf);
      break;
    case kKeys_SaveStateMenu: g_savestate_menu_hotkey = 1; break;
    case kKeys_Rewind: g_rewind_hotkey = 1; break;
    case kKeys_ToggleRenderer:
      g_ppu_render_flags ^= kPpuRenderFlags_NewRenderer;
      printf("New renderer = %x\n", g_ppu_render_flags & kPpuRenderFlags_NewRenderer);
      g_new_ppu = g_ws_active ||
                  (g_ppu_render_flags & kPpuRenderFlags_NewRenderer) != 0;
      break;
    case kKeys_ToggleWidescreen:
      printf("Widescreen is a per-title presentation setting; see the launcher's Mods page.\n");
      break;
    case kKeys_VolumeUp:
    case kKeys_VolumeDown: HandleVolumeAdjustment(j == kKeys_VolumeUp ? 1 : -1); break;
    default: assert(0);
    }
  }
}

static void HandleInput(int keyCode, int keyMod, bool pressed) {
  int j = FindCmdForSdlKey(keyCode, (SDL_Keymod)keyMod);
  if (j != 0)
    HandleCommand(j, pressed);
}

static uint32 GetActiveControllers(void) {
  uint32 ctrl = g_config.has_keyboard_controls;
  ctrl |= g_gamepad[0].joystick_id != -1 ? 1 : 0;
  ctrl |= g_gamepad[1].joystick_id != -1 ? 2 : 0;
  return ctrl << 30;
}

static void OpenOneGamepad(int i) {
  if (!SDL_IsGameController(i)) {
    OpenOneJoystick(i);
    return;
  }
  SDL_GameController *controller = SDL_GameControllerOpen(i);
  if (!controller) {
    fprintf(stderr, "Could not open gamepad %d: %s\n", i, SDL_GetError());
    return;
  }

  uint32 joystick_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
  if (GetGamepadInfo(joystick_id)) {
    SDL_GameControllerClose(controller);
    return;
  }

  uint8 scan_order[3] = { SDL_GameControllerGetPlayerIndex(controller), 0, 1 };

  int found_idx = -1;
  for (int k = 0; k < 3; k++) {
    uint8 j = scan_order[k];
    if (j < 2 && g_config.enable_gamepad[j] && (k == 0 || g_gamepad[j].joystick_id == -1)) {
      found_idx = j;
      break;
    }
  }

  printf("Found controller '%s' assigning to player %d\n", SDL_GameControllerName(controller), found_idx + 1);
  if (found_idx >= 0) {
    GamepadInfo *gi = &g_gamepad[found_idx];
    memset(gi, 0, sizeof(GamepadInfo));
    gi->index = found_idx;
    gi->joystick_id = joystick_id;
  }
}

static void OpenOneJoystick(int i) {
  if (SDL_IsGameController(i)) return;
  SDL_Joystick *joystick = SDL_JoystickOpen(i);
  if (!joystick) {
    fprintf(stderr, "Could not open raw joystick %d: %s\n", i, SDL_GetError());
    return;
  }
  SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
  if (GetGamepadInfo(id)) { SDL_JoystickClose(joystick); return; }
  int slot = -1;
  for (int j = 0; j < 2; ++j) {
    if (g_config.enable_gamepad[j] && g_gamepad[j].joystick_id == -1) {
      slot = j; break;
    }
  }
  if (slot < 0) { SDL_JoystickClose(joystick); return; }
  GamepadInfo *gi = &g_gamepad[slot];
  memset(gi, 0, sizeof(*gi));
  gi->joystick = joystick;
  gi->raw_joystick = true;
  gi->index = slot;
  gi->joystick_id = id;
  printf("Found unmapped raw joystick '%s' assigning to player %d\n",
         SDL_JoystickName(joystick), slot + 1);
}

static int RemapSdlButton(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return kGamepadBtn_A;
  case SDL_CONTROLLER_BUTTON_B: return kGamepadBtn_B;
  case SDL_CONTROLLER_BUTTON_X: return kGamepadBtn_X;
  case SDL_CONTROLLER_BUTTON_Y: return kGamepadBtn_Y;
  case SDL_CONTROLLER_BUTTON_BACK: return kGamepadBtn_Back;
  case SDL_CONTROLLER_BUTTON_GUIDE: return kGamepadBtn_Guide;
  case SDL_CONTROLLER_BUTTON_START: return kGamepadBtn_Start;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return kGamepadBtn_L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return kGamepadBtn_R3;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kGamepadBtn_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kGamepadBtn_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return kGamepadBtn_DpadUp;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kGamepadBtn_DpadDown;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kGamepadBtn_DpadLeft;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kGamepadBtn_DpadRight;
  default: return -1;
  }
}

/* Set/clear a SNES controller bit from a gamepad source. Mirrors
 * HandleCommand's kKeys_Controls / kKeys_ControlsP2 logic but writes
 * to g_pad_buttons so the per-frame keyboard polling can't clobber
 * gamepad-set bits. Non-controller commands (system shortcuts bound
 * via [GamepadMap]) fall through to HandleCommand so things like state
 * save/load on a gamepad button still work. */
static void SetPadButtonOrFallthrough(uint32 j, bool pressed) {
  static const uint8 kKbdRemap[] = { 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
  if (j >= kKeys_Controls && j <= kKeys_Controls_Last) {
    uint32 m = 1u << kKbdRemap[j - kKeys_Controls];
    g_pad_buttons = pressed ? (g_pad_buttons | m) : (g_pad_buttons & ~m);
    return;
  }
  if (j >= kKeys_ControlsP2 && j <= kKeys_ControlsP2_Last) {
    uint32 m = 0x1000u << kKbdRemap[j - kKeys_ControlsP2];
    g_pad_buttons = pressed ? (g_pad_buttons | m) : (g_pad_buttons & ~m);
    return;
  }
  HandleCommand(j, pressed);
}

static void HandleGamepadInput(GamepadInfo *gi, int button, bool pressed) {
  if (!!(gi->modifiers & (1 << button)) == pressed)
    return;
  gi->modifiers ^= 1 << button;
  if (pressed)
    gi->last_cmd[button] = FindCmdForGamepadButton(button + gi->index * kGamepadBtn_Count, gi->modifiers);
  if (gi->last_cmd[button] != 0)
    SetPadButtonOrFallthrough(gi->last_cmd[button], pressed);
}

static void HandleVolumeAdjustment(int volume_adjustment) {
#if SYSTEM_VOLUME_MIXER_AVAILABLE
  int current_volume = GetApplicationVolume();
  int new_volume = IntMin(IntMax(0, current_volume + volume_adjustment * 5), 100);
  SetApplicationVolume(new_volume);
  printf("[System Volume]=%i\n", new_volume);
#else
  g_sdl_audio_mixer_volume = IntMin(IntMax(0, g_sdl_audio_mixer_volume + volume_adjustment * (SNESRECOMP_SDL_MIX_MAXVOLUME >> 4)), SNESRECOMP_SDL_MIX_MAXVOLUME);
  printf("[SDL mixer volume]=%i\n", g_sdl_audio_mixer_volume);
#endif
}

// Approximates atan2(y, x) normalized to the [0,4) range
// with a maximum error of 0.1620 degrees
// normalized_atan(x) ~ (b x + x^2) / (1 + 2 b x + x^2)
static float ApproximateAtan2(float y, float x) {
  uint32 sign_mask = 0x80000000;
  float b = 0.596227f;
  // Extract the sign bits
  uint32 ux_s = sign_mask & *(uint32 *)&x;
  uint32 uy_s = sign_mask & *(uint32 *)&y;
  // Determine the quadrant offset
  float q = (float)((~ux_s & uy_s) >> 29 | ux_s >> 30);
  // Calculate the arctangent in the first quadrant
  float bxy_a = b * x * y;
  if (bxy_a < 0.0f) bxy_a = -bxy_a;  // avoid fabs
  float num = bxy_a + y * y;
  float atan_1q = num / (x * x + bxy_a + num + 0.000001f);
  // Translate it to the proper quadrant
  uint32_t uatan_2q = (ux_s ^ uy_s) | *(uint32 *)&atan_1q;
  return q + *(float *)&uatan_2q;
}

static void HandleGamepadAxisInput(GamepadInfo *gi, int axis, Sint16 value) {
  if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY) {
    *(axis == SDL_CONTROLLER_AXIS_LEFTX ? &gi->last_axis_x : &gi->last_axis_y) = value;
    int buttons = 0;
    if (gi->last_axis_x * gi->last_axis_x + gi->last_axis_y * gi->last_axis_y >= g_config.gamepad_deadzone * g_config.gamepad_deadzone) {
      // in the non deadzone part, divide the circle into eight 45 degree
      // segments rotated by 22.5 degrees that control which direction to move.
      static const uint8 kSegmentToButtons[8] = {
        1 << 4,           // 0 = up
        1 << 4 | 1 << 7,  // 1 = up, right
        1 << 7,           // 2 = right
        1 << 7 | 1 << 5,  // 3 = right, down
        1 << 5,           // 4 = down
        1 << 5 | 1 << 6,  // 5 = down, left
        1 << 6,           // 6 = left
        1 << 6 | 1 << 4,  // 7 = left, up
      };
      uint8 angle = (uint8)(int)(ApproximateAtan2(gi->last_axis_y, gi->last_axis_x) * 64.0f + 0.5f);
      buttons = kSegmentToButtons[(uint8)(angle + 16 + 64) >> 5];
    }
    gi->axis_buttons = buttons;
  } else if ((axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
    if (value < 12000 || value >= 16000)  // hysteresis
      HandleGamepadInput(gi, axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kGamepadBtn_L2 : kGamepadBtn_R2, value >= 12000);
  }
}

/* ── config.ini discovery ─────────────────────────────────────────────────── */

// Go some steps up and find config.ini
static void SwitchDirectory(void) {
  char buf[4096];
  if (!getcwd(buf, sizeof(buf) - 32))
    return;
  size_t pos = strlen(buf);

  for (int step = 0; pos != 0 && step < 3; step++) {
    memcpy(buf + pos, "/config.ini", 12);
    FILE *f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      buf[pos] = 0;
      if (step != 0) {
        printf("Found config.ini in %s\n", buf);
        int err = chdir(buf);
        (void)err;
      }
      return;
    }
    pos--;
    while (pos != 0 && buf[pos] != '/' && buf[pos] != '\\')
      pos--;
  }
}

/* Default config.ini written next to the executable when none was
 * discoverable on launch. The [GamepadMap] section gives a plugged-in Xbox
 * controller working defaults out of the box. A title overrides the whole
 * text through SnesDesktopHostGame.default_config_ini. */
static const char kDefaultConfigIniContent[] =
  "[General]\n"
  "# Automatically save state on quit and reload on start\n"
  "Autosave = 0\n"
  "\n"
  "# Disable the SDL_Delay that happens each frame (slightly better\n"
  "# perf if your display is set to exactly 60hz)\n"
  "DisableFrameDelay = 0\n"
  "\n"
  "[Graphics]\n"
  "# Window size (Auto or WidthxHeight)\n"
  "WindowSize = Auto\n"
  "\n"
  "# Fullscreen mode (0=windowed, 1=desktop fullscreen, 2=fullscreen w/mode change)\n"
  "Fullscreen = 0\n"
  "\n"
  "# Window scale (1=100%, 2=200%, 3=300%, etc.)\n"
  "WindowScale = 3\n"
  "\n"
  "# Use the optimized SNES PPU implementation\n"
  "NewRenderer = 1\n"
  "\n"
  "# Don't keep the aspect ratio\n"
  "IgnoreAspectRatio = 0\n"
  "\n"
  "# Remove the sprite limits per scan line\n"
  "NoSpriteLimits = 1\n"
  "\n"
  "[Sound]\n"
  "EnableAudio = 1\n"
  "AudioFreq = 32000\n"
  "AudioChannels = 2\n"
  "AudioSamples = 512\n"
  "\n"
  "[KeyMap]\n"
  "# This section is for system-level shortcuts (save/load state,\n"
  "# fullscreen, pause, etc.). The 12 SNES controller buttons live\n"
  "# in keybinds.ini next to the executable.\n"
  "Fullscreen = Alt+Return\n"
  "Reset = Ctrl+r\n"
  "Pause = Shift+p\n"
  "PauseDimmed = p\n"
  "Turbo = Tab\n"
  "WindowBigger = Ctrl+Up\n"
  "WindowSmaller = Ctrl+Down\n"
  "VolumeUp = Shift+=\n"
  "VolumeDown = Shift+-\n"
  "DisplayPerf = f\n"
  "ToggleRenderer = r\n"
  "SaveStateMenu = F11\n"
  "Rewind = F8\n"
  "Load =      F1,     F2,     F3,     F4,     F5,     F6,     F7,     F8,     F9,     F10\n"
  "Save = Shift+F1,Shift+F2,Shift+F3,Shift+F4,Shift+F5,Shift+F6,Shift+F7,Shift+F8,Shift+F9,Shift+F10\n"
  "\n"
  "[GamepadMap]\n"
  "# Enable each player's gamepad slot. SDL_GameController-compatible\n"
  "# controllers (Xbox, PlayStation, Switch Pro, etc.) auto-detect\n"
  "# when plugged in. Set to false to force keyboard-only.\n"
  "EnableGamepad1 = true\n"
  "EnableGamepad2 = true\n"
  "\n"
  "# Default Xbox-layout mapping. Order matches kKeys_Controls:\n"
  "#   Up, Down, Left, Right, Select, Start, A, B, X, Y, L, R\n"
  "# Edit + restart to rebind. Shoulder = L1/Lb (top), trigger = L2.\n"
  "Controls =   DpadUp, DpadDown, DpadLeft, DpadRight, Back, Start, B, A, Y, X, Lb, Rb\n"
  "ControlsP2 = DpadUp, DpadDown, DpadLeft, DpadRight, Back, Start, B, A, Y, X, Lb, Rb\n";

static const char *DefaultConfigIni(void) {
  return g_game->default_config_ini ? g_game->default_config_ini : kDefaultConfigIniContent;
}

/* Write the default config.ini next to the executable and chdir there. Silent
 * no-op if it can't derive the exe directory from `exe_path`. */
static void WriteDefaultConfigIni(const char *exe_path) {
  if (!exe_path || !*exe_path) return;
  const char *slash = NULL;
  for (const char *p = exe_path; *p; p++)
    if (*p == '/' || *p == '\\') slash = p;
  if (!slash) return;
  size_t dir_len = (size_t)(slash - exe_path);
  if (dir_len + 12 >= 1024) return;  /* path too long */
  char dir[1024];
  memcpy(dir, exe_path, dir_len);
  dir[dir_len] = 0;
  char ini_path[1024];
  snprintf(ini_path, sizeof(ini_path), "%s/config.ini", dir);
  FILE *f = fopen(ini_path, "w");
  if (!f) {
    fprintf(stderr, "Warning: could not write default config.ini to %s\n", ini_path);
    return;
  }
  fputs(DefaultConfigIni(), f);
  fclose(f);
  printf("[config.ini] Generated %s\n", ini_path);
  if (chdir(dir) != 0) {
    fprintf(stderr, "Warning: could not chdir to %s\n", dir);
  }
}

/* Ensure config.ini is reachable from cwd. SwitchDirectory walks up to
 * 3 levels looking for one and chdir's if it finds it; if it didn't,
 * write a default so first-launch from a clean release directory always
 * has a working config. */
static void EnsureConfigIniNextToExe(const char *exe_path) {
  FILE *f = fopen("config.ini", "rb");
  if (f) {
    fclose(f);
    return;
  }
  /* Prefer the anchored cwd: snesrecomp_anchor_to_exe_dir() has already pointed
   * it at the .AppImage's folder (or the exe dir on Windows). Deriving the
   * directory from argv[0] instead resolves inside the read-only AppImage
   * mount, where the write silently fails. */
  f = fopen("config.ini", "w");
  if (f) {
    fputs(DefaultConfigIni(), f);
    fclose(f);
    printf("[config.ini] Generated config.ini in the anchored directory\n");
    return;
  }
  /* Anchor declined (read-only install): fall back to the exe-relative write. */
  WriteDefaultConfigIni(exe_path);
}
