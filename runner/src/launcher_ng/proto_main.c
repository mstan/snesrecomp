// proto_main.c — standalone prototype driver.
//
// Game-agnostic by construction: it fabricates the SAME C ABI structs a real
// game passes (here seeded with Mega Man X's real facts and panel gating), then
// runs whichever backend is compiled in. This is the throwaway harness for the
// Clay-vs-ImGui bake-off; the production path replaces it with the real
// snes_launcher_run_window() implemented once in the engine.

#include "launcher_backend.h"
#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_theme.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // ---- seed the C ABI structs exactly as MegamanXRecomp/src/main.c does ----
    SnesLauncherCSettings s;
    memset(&s, 0, sizeof(s));
    s.output_method  = 2;      // OpenGL
    s.window_scale   = 3;
    s.fullscreen     = 0;
    s.linear_filter  = 0;
    s.widescreen     = 0;
    s.enable_audio   = 1;
    s.audio_freq     = 32000;
    s.volume         = 100;
    s.player_src[0]  = 1;      // keyboard
    s.player_src[1]  = 0;      // none
    s.skip_launcher  = 0;

    SnesLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    gi.name                 = "Mega Man X";
    gi.region               = "USA";
    gi.has_expected_crc     = 1;
    gi.expected_crc         = 0x1B4B2E9Cu;   // representative; real value verified in prod
    gi.widescreen_supported = 0;             // MMX: hide widescreen (matches main.c:854)
    gi.msu1_supported       = 0;             // MMX: hide MSU-1 (matches main.c:855)
    gi.sram_path            = NULL;          // MMX: password game, no SAVES panel

    LauncherModel model;
    launcher_model_init(&model, &s, &gi, "mmx.sfc");

    LauncherTheme theme = launcher_theme_default();

    char title[128];
    snprintf(title, sizeof(title), "Mega Man X — Launcher [%s]", launcher_backend_name());

    LauncherPlatform plat;
    if (!launcher_platform_open(&plat, title, 1100, 720)) {
        fprintf(stderr, "[proto] platform init failed; a real game would boot as if skipped.\n");
        return 2;
    }

    LngAction act = launcher_backend_run(&plat, &model, &theme);
    launcher_platform_close(&plat);

    // In production this is the value snes_launcher_run_window() returns to
    // main.c, which then boots the game IN-PROCESS with the committed settings
    // (0=LAUNCH, 1=QUIT). The standalone prototype just reports it.
    if (act == LNG_ACTION_LAUNCH) {
        launcher_model_commit(&model, &s);
        printf("[proto] LAUNCH  scale=%s filter=%d freq=%d skip=%d rom=%s\n",
               launcher_model_scale_label(&model), s.linear_filter,
               s.audio_freq, s.skip_launcher, launcher_model_rom_path(&model));
    } else {
        printf("[proto] QUIT\n");
    }
    return 0;
}
