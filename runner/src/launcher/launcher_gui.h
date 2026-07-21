// launcher_gui.h — shared RmlUi pre-boot launcher for snesrecomp games.
//
// Shown inside the runner's SDL/OpenGL window before the emulator boots, modeled
// on psxrecomp/runtime/launcher. The user picks/verifies a ROM, tunes settings
// (Display / Audio / Widescreen / Controllers / MSU-1), then presses PLAY. The
// chosen values are written back into the caller-owned SnesLauncherSettings (and
// the resolved ROM path), which the game's main.c maps onto its own Config and
// persists to config.ini / keybinds.ini.
//
// Design note (same as the PSX launcher): this module does NOT own the window or
// GL context — the caller passes an already-current GL 3.3 core context. That
// keeps it a pure overlay so a future "reopen settings while running" path can
// reuse it without owning the window lifecycle.
//
// The launcher is game-AGNOSTIC: it edits the shared subset below, never a
// game's private Config struct. Each game's main.c does the (tiny) mapping.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct SDL_Window;

namespace snes_launcher {

enum class Result {
    Launch,       // user pressed PLAY / netplay launch — boot with out_rom_path
    Quit,         // user closed the window — caller should exit
    Unavailable,  // launcher could not initialise (assets/GL) — caller boots as if skipped
};

// Per-player input source. Mirrors the dashboard's controller dropdown.
enum class InputSource : int {
    None     = 0,
    Keyboard = 1,
    Gamepad  = 2,
};

// Filled on Result::Launch when a netplay lobby session is ready to boot.
struct NetplayLaunch {
    bool     enabled = false;
    uint32_t session_id = 0;
    int      local_slot = 0;
    char     bind_hostport[64] = {0};
    char     peer_hostport[64] = {0};
    std::string display_name;
    int      transport = 0; /* 0 auto, 1 ICE, 2 LAN */
    int      input_delay = 2; /* recomp-net frames; from host match_caps */
    int      ws_extra = -1;   /* from match_caps; -1 = unset (legacy Phase 2a) */
};

// The editable settings subset. Seeded by the caller from its Config, mutated in
// place, and read back on Result::Launch. Field set is intentionally the union of
// what every SNES game exposes; a game ignores fields it doesn't use.
struct SnesLauncherSettings {
    // --- Display ---
    int  output_method   = 2;     // kOutputMethod_OpenGL by convention (0 SDL,1 SDL-SW,2 GL)
    int  window_scale    = 2;     // 1..N integer scale
    int  fullscreen      = 0;     // 0 windowed, 1 desktop-fs, 2 exclusive-fs
    bool ignore_aspect   = false;
    bool linear_filter   = false;

    // --- Widescreen (EXPERIMENTAL, default off => authentic 256-wide) ---
    bool widescreen      = false;
    bool widescreen_hud  = true;

    // --- Audio ---
    bool enable_audio    = true;
    int  audio_freq      = 48000;
    int  volume          = 100;   // 0..100 (game maps to its own scale)

    // --- Controllers (2 players) ---
    InputSource player_src[2] = { InputSource::Keyboard, InputSource::Gamepad };
    // Per-player device token persisted across refresh / relaunch:
    // "none", "keyboard", or an SDL joystick GUID hex string. Empty means
    // "derive from player_src + first available pad" (legacy / first launch).
    char player_device[2][40] = {{0}, {0}};
    int  deadzone[2]     = { 30, 30 };  // 0..100 percent of stick range

    // --- Launcher behaviour ---
    bool skip_launcher   = false;       // boot straight to the game on subsequent
                                        // launches (pass --launcher, or set
                                        // SkipLauncher = 0 in config.ini, to force
                                        // the launcher back).

    // --- MSU-1 ---
    bool msu1_enabled    = true;        // use streamed audio when a pack is present
    char msu1_dir[512]   = {0};         // directory holding <name>-N.pcm / <name>.msu

    // --- Netplay ---
    char netplay_player_name[64] = {0}; // lobby display name (config.ini NetplayPlayerName)
};

// Static facts about the game being configured. Drives the title, the ROM
// verification badge, and the MSU-1 panels.
struct GameInfo {
    const char* name             = nullptr;  // "Super Mario World"
    const char* region           = nullptr;  // "(USA)"
    const char* boxart_png       = nullptr;  // optional asset filename under img/

    // SRAM save file this game reads/writes, exe-dir anchored (e.g.
    // "saves/Super Mario World.srm"). Drives the SAVES panel (found/size) and the
    // Import / Clear Save actions. Null => the SAVES panel stays inert.
    const char* sram_path        = nullptr;

    // ROM verification. CRC32 is the quick badge; the SHA-256 list is the set of
    // accepted ROMs (vanilla + MSU-1-patched). Either may be empty.
    uint32_t    expected_crc     = 0;
    bool        has_expected_crc = false;
    const uint8_t (*known_sha256)[32] = nullptr;  // array of 32-byte digests
    size_t      num_known_sha256 = 0;

    // Whether this game has the widescreen (16:9) path. When false the Settings
    // → Widescreen panel is hidden entirely (e.g. MMX, which has no widescreen).
    bool        widescreen_supported = true;

    // Local controller rows on the offline dashboard (1 or 2). 1 hides Player 2
    // (no local multiplayer). 0 = treat as 2.
    int         num_players = 2;

    // When set: widescreen is mandatory — Settings toggle hidden, expand forced
    // on, and match_caps.ws_extra is always force_ws_extra (default 71).
    bool        force_widescreen = false;
    int         force_ws_extra = 71;

    // MSU-1. msu1_supported drives ALL MSU-1 UI: when false the dashboard flag
    // AND the Settings → Audio MSU-1 block are hidden entirely (e.g. a game with
    // no MSU-1 driver baked in). When true, msu1_note is shown under the MSU-1
    // settings to name the exact patch a pack must match (per-game, since SMW
    // alone has three incompatible MSU-1 patches).
    bool        msu1_supported   = false;
    const char* msu1_note        = nullptr;  // e.g. "Audio-only 'SMW MSU-1' (t1436)"
    const char* msu1_patch_path  = nullptr;  // bundled .ips (null = no runtime patch)

    // config.ini path for the Settings → Hotkeys editor ([KeyMap] section,
    // surgical read-modify-write). Null => "config.ini" in cwd (exe-anchored).
    const char* config_path      = nullptr;
};

struct RunOptions {
    /* Re-open on the netplay room view after a soft return from a match
     * (lobby WebSocket must still be connected). */
    bool resume_netplay_room = false;
};

// Run the launcher loop to completion. `gl_context` is an SDL_GLContext (void*)
// already created and current on `window`. `io` is seeded with the effective
// settings and, on Result::Launch, updated in place. `assets_dir` holds
// launcher.rml / fonts / img. On Result::Launch, `out_rom_path` receives the
// ROM to boot (possibly the .msu1 patched copy). `initial_rom` may be a cached
// path (rom.cfg) to pre-populate the dashboard, or null/empty.
// When a netplay lobby launches, `out_net` (if non-null) is filled.
Result run(SDL_Window* window, void* gl_context,
           SnesLauncherSettings& io, const GameInfo& game,
           const char* assets_dir, const char* initial_rom,
           char* out_rom_path, size_t out_rom_path_len,
           NetplayLaunch* out_net = nullptr,
           const RunOptions& opts = RunOptions{});

} // namespace snes_launcher
