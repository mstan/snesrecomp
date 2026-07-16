// launcher_files.h — platform-agnostic ROM picker (shared core).
//
// Uses tinyfiledialogs (zlib) so ONE implementation gets a real native picker
// on Windows (GetOpenFileName), macOS (NSOpenPanel/osascript) and Linux
// (zenity / kdialog / yad / qarma). This replaces the old launcher's
// pick_file(), which was a Win32-only implementation with a
// `return false;` stub on every other platform — i.e. "Change ROM" silently did
// nothing on Linux and macOS.
//
// Deliberately SDL-version agnostic: it does not depend on SDL3's
// SDL_ShowOpenFileDialog, so it works identically on the SDL2 build we ship now
// and the SDL3 build that follows.

#ifndef LAUNCHER_NG_FILES_H
#define LAUNCHER_NG_FILES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open the OS "choose a ROM" dialog (BLOCKING — returns when the user picks or
// cancels). Returns true and fills `out_path` on success.
bool launcher_pick_rom(char* out_path, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_FILES_H
