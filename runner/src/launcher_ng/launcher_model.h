// launcher_model.h — game-agnostic view-model for the next-gen launcher.
//
// This is the DRY heart of the new launcher: it owns all launcher STATE and
// BEHAVIOR (which panels exist, what a control does, how a rebind is captured)
// and is completely free of any UI toolkit, SDL, or OpenGL. Both prototype
// render backends (Dear ImGui and Clay) draw this same model and call the same
// mutators, so behavior is identical across backends and — because it is built
// purely from the existing C ABI structs (SnesLauncherCSettings /
// SnesLauncherCGameInfo) — identical across every game in the ecosystem.
//
// The surface mirrors the shipping RmlUi MMX launcher (launcher.rml) so the
// prototype is a faithful parity check of what we offer the end user:
//   Dashboard  : game/ROM info + CRC/SHA badges + Change ROM + controllers
//   Settings   : window scale, linear filter, sample rate, volume, hotkeys
//   Controller : input source, deadzone, keyboard rebinds
//   Footer     : Skip-on-Boot (+confirm modal), Settings/Back, PLAY
// Per-game gating (widescreen/MSU-1/saves) hides panels exactly as today.

#ifndef LAUNCHER_NG_MODEL_H
#define LAUNCHER_NG_MODEL_H

#include "launcher/launcher_capi.h"   // SnesLauncherCSettings, SnesLauncherCGameInfo

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LNG_VIEW_DASHBOARD = 0,
    LNG_VIEW_SETTINGS,
    LNG_VIEW_CONTROLLER,
} LngView;

typedef enum {
    LNG_ACTION_NONE = 0,   // still running
    LNG_ACTION_LAUNCH,     // boot the game with committed settings
    LNG_ACTION_QUIT        // user quit
} LngAction;

// Representative subset of the SNES pad for the rebind UI.
typedef enum {
    LNG_BTN_UP = 0, LNG_BTN_DOWN, LNG_BTN_LEFT, LNG_BTN_RIGHT,
    LNG_BTN_A, LNG_BTN_B, LNG_BTN_X, LNG_BTN_Y,
    LNG_BTN_L, LNG_BTN_R, LNG_BTN_START, LNG_BTN_SELECT,
    LNG_BTN_COUNT
} LngButton;

// System hotkeys ([KeyMap] section) shown on the Settings page.
typedef enum {
    LNG_HK_PAUSE = 0, LNG_HK_RESET, LNG_HK_FASTFWD, LNG_HK_FULLSCREEN,
    LNG_HK_SCREENSHOT, LNG_HK_QUIT,
    LNG_HK_COUNT
} LngHotkey;

typedef struct {
    // ---- static game facts (borrowed from SnesLauncherCGameInfo) ----
    const char* game_name;          // e.g. "Mega Man X"
    const char* region;             // e.g. "USA"
    bool        widescreen_supported;
    bool        msu1_supported;
    bool        saves_supported;     // sram_path != NULL -> show the SAVES panel
    const char* sram_path;           // borrowed; NULL when the game has no SRAM
    // Number of players the GAME actually supports. Mega Man X is 1-player, so
    // the launcher must not show a dead Player 2 row. Games that support 2
    // report 2 and the second row appears. Driven by data, never hardcoded.
    int         player_count;

    // ---- ROM verification (synthesized in prototype; real impl uses crc32/sha256) ----
    bool     rom_present;
    char     rom_full[512];          // absolute path (what we hand to the game)
    char     rom_file[128];          // basename for display, e.g. "mmx.sfc"
    char     rom_size[48];           // "1.50 MB"
    char     rom_header[24];         // "LoROM"
    char     rom_crc_str[16];        // "1B4B2E9C"
    char     rom_sha_str[24];        // "9c2e…d41f"
    bool     crc_match;
    bool     sha_match;

    // ---- editable settings (working copy of the C ABI struct) ----
    SnesLauncherCSettings s;

    // ---- transient UI state ----
    LngView   view;
    LngAction action;
    int       cfg_player;            // 0/1 — which player the Controller view edits
    bool      skip_modal_open;       // "Skip the launcher on boot?" confirm

    // Selected gamepad per player (when player_src == 2). pad_id is the live
    // SDL_JoystickID; name is cached for display if the device disconnects.
    uint32_t  player_pad_id[2];
    char      player_pad_name[2][64];

    // rebind capture state machine
    bool      capturing;
    LngButton capture_btn;
    char      binds[2][LNG_BTN_COUNT][32];  // per-player binding labels
    char      hotkeys[LNG_HK_COUNT][24];    // [KeyMap] labels
} LauncherModel;

// Build the model from the inbound C ABI structs. `initial_rom` may be NULL.
void launcher_model_init(LauncherModel* m,
                         const SnesLauncherCSettings* io,
                         const SnesLauncherCGameInfo* game,
                         const char* initial_rom);

// Copy the working settings back into the caller's struct (on LAUNCH).
void launcher_model_commit(const LauncherModel* m, SnesLauncherCSettings* io);

// Adopt a newly-picked ROM path (from the native file dialog): updates the
// displayed file name / verification state.
void launcher_model_set_rom(LauncherModel* m, const char* path);

// Full path of the currently selected ROM ("" when none).
const char* launcher_model_rom_path(const LauncherModel* m);

// ---- navigation ----
void launcher_model_set_view(LauncherModel* m, LngView v);
void launcher_model_open_config(LauncherModel* m, int player);  // -> Controller view

// ---- display settings ----
void launcher_model_cycle_scale(LauncherModel* m);   // 1..6 wrap
void launcher_model_toggle_filter(LauncherModel* m);
void launcher_model_toggle_widescreen(LauncherModel* m);  // gated

// ---- audio settings ----
void launcher_model_cycle_freq(LauncherModel* m);    // 32000/44100/48000
void launcher_model_volume_delta(LauncherModel* m, int delta);  // clamp 0..100

// ---- controllers ----
void launcher_model_cycle_player_src(LauncherModel* m, int player); // None/Kbd/Pad
void launcher_model_deadzone_delta(LauncherModel* m, int player, int delta);
// Set the input source explicitly (used by the device dropdown). kind: 0 None,
// 1 Keyboard, 2 Gamepad. For gamepad, pass the SDL id + display name.
void launcher_model_set_source(LauncherModel* m, int player, int kind,
                               uint32_t pad_id, const char* pad_name);

// ---- skip-on-boot (footer switch + confirm modal) ----
void launcher_model_request_skip_toggle(LauncherModel* m); // opens modal when enabling
void launcher_model_skip_confirm(LauncherModel* m);
void launcher_model_skip_cancel(LauncherModel* m);

// ---- rebind capture ----
void launcher_model_begin_capture(LauncherModel* m, LngButton b);
void launcher_model_accept_capture(LauncherModel* m, const char* label);
void launcher_model_cancel_capture(LauncherModel* m);
void launcher_model_reset_binds(LauncherModel* m);   // current player -> defaults

// ---- display-string helpers (single source of truth across backends) ----
const char* launcher_model_scale_label(const LauncherModel* m);        // "3x"
const char* launcher_model_freq_label(const LauncherModel* m);         // "44100 Hz"
const char* launcher_model_player_src_label(const LauncherModel* m, int player);
const char* launcher_button_name(LngButton b);
const char* launcher_hotkey_name(LngHotkey h);
const char* launcher_view_name(LngView v);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_MODEL_H
