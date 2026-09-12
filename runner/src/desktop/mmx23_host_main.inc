/* Compatibility adapter for the X2/X3 identity shims. The desktop host,
 * menus, audio, input and presentation loop are owned by host_main.c. */
#include <stdlib.h>
#include "desktop/host_main.h"
#include "desktop/config.h"
#include "snes/ppu.h"
#include "widescreen.h"
#include MMX_RTL_HEADER
#include MMX_DISPLAY_HEADER
#include MMX_SPC_HEADER

#ifndef SNESRECOMP_BUILD_VERSION
#define SNESRECOMP_BUILD_VERSION "dev"
#endif
#ifndef MMX_DESKTOP_ENTRY
#define MMX_DESKTOP_ENTRY main
#endif
extern const RtlGameInfo kMmxGameInfo;

static void MmxPrepareFrame(int dw, int dh, int *w, int *h) {
  *w = MmxDisplay_ComputeFrameWidth(dw, dh, g_config.widescreen);
  *h = 224;
}
static void MmxBeginFrame(unsigned number) {
  (void)number;
  /* 16:9 HUD anchoring: measured slot map in docs/OAM_SURVEY.md.
   * HP (0-5, X=8) and weapon (7-13, X=24) anchor LEFT; the boss
   * bar (16-22, X=232) anchors RIGHT, so they move in opposite
   * directions. Gated on the HUD signature being present in OAM,
   * because cutscenes reuse slots 0-23 for actors. */
  MmxConfigureWsHud();
  /* BG3 is normally a HUD/text layer and stays clamped to the native 256
   * columns. Wire Sponge's weather overlays (rain, heat shimmer) are the
   * exception: gameplay puts BG3 ALONE on the SUBSCREEN (TS=$04, TM lacks
   * BG3) and blends it via color math — measured from the owner's rain
   * save (BG3SC=$0C, 32x32, drifting scroll). For exactly that signature,
   * fill the margins by cyclically REPEATING the authentic native line
   * (PpuSetWidescreenLayerRepeat) rather than map wrap: the rain texture
   * is patchy, and wrap can seat a sparse patch in a gutter against a
   * dense native column (owner-reported corner gaps). Repeat keeps gutter
   * density identical to the adjacent native rain by construction. Menus
   * (BG3 on the main screen) keep the authentic clamp. */
#if MMX_HAS_BG3_SUB_OVERLAY
  {
    static int s_bg3_enabled = -1;
    if (s_bg3_enabled < 0) {
      const char *e = getenv("SNESRECOMP_WS_BG3");
      s_bg3_enabled = (e && e[0] == '0') ? 0 : 1;
    }
    bool bg3_sub_overlay = s_bg3_enabled &&
                           g_ppu->screenEnabled[1] == 0x04 &&
                           !(g_ppu->screenEnabled[0] & 0x04);
    PpuSetWidescreenLayerRepeat(g_ppu, bg3_sub_overlay ? 0x04 : 0x00);
  }
#else
  PpuSetWidescreenBg3Widen(g_ppu, 0);
#endif
  PpuSetWidescreenLineEnhancer(g_ppu, NULL, NULL);
  MmxConfigureWsBgMargins();
}

void MmxDisplay_SetWidescreenEnabled(bool enabled) {
  snesrecomp_desktop_set_widescreen(enabled);
}
bool MmxDisplay_IsWidescreenEnabled(void) { return g_config.widescreen; }
bool MmxDisplay_IsWidescreenActive(void) { return g_ws_active; }
int MmxDisplay_GetCurrentFrameWidth(void) { return snesrecomp_desktop_frame_width(); }

int MMX_DESKTOP_ENTRY(int argc, char **argv) {
  ConfigUseStateMenuDefaults();
  static const SnesDesktopHostGame game = {
    .display_name = MMX_GAME_NAME,
    .window_title = MMX_WINDOW_TITLE,
    .launcher_title = MMX_LAUNCHER_TITLE,
    .region = MMX_GAME_REGION,
    .rom_file = MMX_ROM_FILE,
    .expected_sha256_hex = MMX_ROM_SHA256_HEX,
    .expected_crc32_hex = MMX_ROM_CRC32_HEX,
    .game_id = MMX_MOD_GAME_ID,
    .build_version = SNESRECOMP_BUILD_VERSION,
    .game_info = &kMmxGameInfo,
    .debug_port = MMX_DEBUG_PORT,
    .widescreen_supported = 1,
    .native_widescreen = 1,
    .state_menu_hotkeys = 1,
    .simulation_hz = 60.0,
    .create_spc_player = SmwSpcPlayer_Create,
    .prepare_frame = MmxPrepareFrame,
    .begin_sim_frame = MmxBeginFrame,
    .window_base_width = MmxDisplay_GetWindowBaseWidth,
    .window_base_height = MmxDisplay_GetWindowBaseHeight,
  };
  return snesrecomp_desktop_main(&game, argc, argv);
}
