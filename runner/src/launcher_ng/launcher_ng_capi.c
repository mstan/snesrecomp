// launcher_ng_capi.c — the C ABI the game calls, backed by the launcher_ng UI.
//
// Implements snes_launcher_run_window() (declared in launcher/launcher_capi.h),
// the SAME entry point main.c already calls at main.c:858. Dropping this in
// (and dropping the old RmlUi launcher_gui.cpp from the build) swaps the launcher
// with ZERO change to main.c: it still seeds the C structs, calls the function,
// and reads back the chosen ROM + settings.
//
// Returns: 0 = LAUNCH, 1 = QUIT, 2 = UNAVAILABLE (assets/GL failed → boot as if
// skipped), matching launcher_capi.h.

#include "launcher/launcher_capi.h"

#include "launcher_backend.h"
#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_theme.h"

#include <stdio.h>
#include <string.h>

int snes_launcher_run_window(const char* window_title,
                             SnesLauncherCSettings* io,
                             const SnesLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len) {
    (void)assets_dir;   // launcher_ng resolves assets next to the exe (SDL base path)

    LauncherPlatform plat;
    if (!launcher_platform_open(&plat, window_title ? window_title : "Launcher",
                                1100, 720)) {
        // Window/GL init failed — tell the caller to boot as if the launcher was
        // skipped, exactly like the old launcher's UNAVAILABLE path.
        return 2;
    }

    LauncherModel model;
    launcher_model_init(&model, io, game, initial_rom);

    LauncherTheme theme = launcher_theme_default();

    LngAction act = launcher_backend_run(&plat, &model, &theme);

    launcher_platform_close(&plat);

    if (act == LNG_ACTION_LAUNCH) {
        launcher_model_commit(&model, io);   // edited settings back to the caller
        const char* rom = launcher_model_rom_path(&model);
        if (out_rom_path && out_rom_path_len) {
            if (rom && rom[0])
                snprintf(out_rom_path, out_rom_path_len, "%s", rom);
            else if (initial_rom)
                snprintf(out_rom_path, out_rom_path_len, "%s", initial_rom);
            else
                out_rom_path[0] = '\0';
        }
        return 0;   // LAUNCH
    }

    return 1;       // QUIT
}
