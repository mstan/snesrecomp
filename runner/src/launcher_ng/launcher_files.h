// launcher_files.h — platform-agnostic ROM picker + game handoff (shared core).
//
// Uses SDL3's native dialog + process APIs, so a single implementation gets the
// real OS file picker on Windows (IFileDialog), macOS (NSOpenPanel) and Linux
// (xdg-desktop-portal, with a Zenity fallback) — no per-platform code, no extra
// dependency. This is the game-agnostic version every recomp reuses.

#ifndef LAUNCHER_NG_FILES_H
#define LAUNCHER_NG_FILES_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the OS "choose a ROM" dialog (non-blocking; the callback fires on the
// main thread during event pumping). `parent` may be NULL.
// On success, `out_path` (cap `out_cap`) is filled and `*done` set true.
void launcher_pick_rom(SDL_Window* parent, char* out_path, size_t out_cap, bool* done);

// Launch the real game executable, handing it the chosen ROM. Returns true if
// the process was spawned. `exe_path` is the game binary; `rom_path` may be
// NULL (the game then resolves its ROM as usual).
bool launcher_launch_game(const char* exe_path, const char* rom_path);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_FILES_H
