// launcher_binds.h — real input-binding persistence for the launcher.
//
// Bridges the launcher's Controller view to the engine's persisted files:
//   * player buttons  -> keybinds.ini  (SDL *scancode* names), via keybinds.c
//   * system hotkeys   -> config.ini [KeyMap] (SDL *keycode* names), surgical edit
//
// This is the module that makes remaps actually STICK. Kept separate from the
// pure view-model so the model stays SDL/engine-free; the backend calls these
// on capture, and the capi/proto driver calls launcher_binds_load() at startup.

#ifndef LAUNCHER_NG_BINDS_H
#define LAUNCHER_NG_BINDS_H

#include "launcher_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load current bindings from disk into the model for DISPLAY. Initializes
// keybinds.ini (generating defaults if absent) and reads config.ini [KeyMap].
// config_path may be NULL (=> "config.ini" in the exe dir).
void launcher_binds_load(LauncherModel* m, const char* config_path);

// A player button was rebound to `scancode` (an SDL_Scancode). Persist to
// keybinds.ini and refresh the model's display string.
void launcher_binds_set_button(LauncherModel* m, int player, LngButton b, int scancode);

// Reset one player's keyboard bindings to defaults and persist.
void launcher_binds_reset_player(LauncherModel* m, int player);

// A system hotkey was rebound. `keycode` is an SDL_Keycode, `kmod` the SDL
// modifier mask; pass keycode==0 to UNBIND. Persists to config.ini [KeyMap]
// and refreshes the model's display string.
void launcher_binds_set_hotkey(LauncherModel* m, LngHotkey h, int keycode, int kmod);

// The config.ini path hotkeys are written to (NULL => default). Set once at load.
extern const char* g_launcher_config_path;

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_BINDS_H
