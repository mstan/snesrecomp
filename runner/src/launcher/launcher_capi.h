// launcher_capi.h — C-callable entry point for the RmlUi launcher.
//
// main.c (C) can't speak the C++ snes_launcher::run() API directly, so this
// shim wraps it: it creates its own SDL/GL window, runs the launcher, maps a
// plain-C settings struct in/out, and tears the window down — leaving main.c to
// just seed/read the struct and pick up the chosen ROM path.

#ifndef SNESRECOMP_LAUNCHER_CAPI_H
#define SNESRECOMP_LAUNCHER_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors snes_launcher::SnesLauncherSettings as plain C (bools as int).
typedef struct SnesLauncherCSettings {
    int  output_method;     // 0 SDL, 1 SDL-software, 2 OpenGL
    int  window_scale;      // 1..N
    int  fullscreen;        // 0 off, 1 borderless, 2 exclusive
    int  ignore_aspect;     // bool
    int  linear_filter;     // bool
    int  widescreen;        // bool (EXPERIMENTAL, default 0)
    int  widescreen_hud;    // bool
    int  enable_audio;      // bool
    int  audio_freq;        // Hz
    int  volume;            // 0..100
    int  player_src[2];     // 0 none, 1 keyboard, 2 gamepad
    /* "none" / "keyboard" / SDL joystick GUID (or empty = derive). */
    char player_device[2][40];
    int  deadzone[2];       // 0..100
    int  skip_launcher;     // bool: boot straight to the game next time
    int  msu1_enabled;      // bool
    char msu1_dir[512];
    char netplay_player_name[64]; /* lobby display name */
} SnesLauncherCSettings;

/* Filled on LAUNCH when a netplay lobby session is ready for the host loop. */
typedef struct SnesNetplayLaunch {
    int      enabled;           /* bool */
    uint32_t session_id;
    int      local_slot;
    char     bind_hostport[64];
    char     peer_hostport[64];
    char     display_name[64];
    /* 0 = auto (private peer → LAN, else ICE), 1 = ICE, 2 = LAN */
    int      transport;
    int      input_delay;       /* recomp-net frames (host match_caps) */
    int      ws_extra;          /* from match_caps; -1 = unset (legacy Phase 2a) */
} SnesNetplayLaunch;

typedef struct SnesLauncherCGameInfo {
    const char*    name;
    const char*    region;
    uint32_t       expected_crc;
    int            has_expected_crc;
    const uint8_t (*known_sha256)[32];
    size_t         num_known_sha256;
    int            widescreen_supported;   /* hide Widescreen settings when 0 */
    /* How many players the GAME supports (1 or 2). The launcher hides the
     * Player 2 row entirely when this is 1 — e.g. Mega Man X is 1-player, so a
     * P2 row is dead UI. 0 means "unset" and is treated as 2 for backward
     * compatibility with callers that predate this field. */
    int            num_players;
    /* When non-zero: widescreen is mandatory for this title. Hide the Settings
     * toggle, force expand on, and put force_ws_extra (or 71 if 0) into every
     * lobby match_caps so peers cannot desync on g_ws_extra. */
    int            force_widescreen;
    int            force_ws_extra;
    int            msu1_supported;
    const char*    msu1_note;          /* shown under MSU-1 settings (which patch) */
    const char*    msu1_patch_path;
    const char*    sram_path;          /* "saves/<title>.srm" (exe-anchored) for SAVES panel */
    /* config.ini path the hotkey editor reads/writes ([KeyMap] section only,
     * surgical edits). NULL => "config.ini" in cwd (exe-anchored by main).
     * Games pass their --config override here so hotkey edits follow it. */
    const char*    config_path;
} SnesLauncherCGameInfo;

// Returns: 0 = LAUNCH (boot out_rom_path with the edited *io),
//          1 = QUIT (caller should exit),
//          2 = UNAVAILABLE (assets/GL failed — caller boots as if skipped).
// out_net may be NULL; when non-NULL and a lobby launch occurs, it is filled.
// resume_netplay_room: non-zero re-opens the MotK room view (lobby WS must
// still be connected after a soft return from an in-game match).
int snes_launcher_run_window(const char* window_title,
                             SnesLauncherCSettings* io,
                             const SnesLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len,
                             SnesNetplayLaunch* out_net,
                             int resume_netplay_room);

#ifdef __cplusplus
}
#endif

#endif // SNESRECOMP_LAUNCHER_CAPI_H
