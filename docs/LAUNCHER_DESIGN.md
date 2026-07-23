# snesrecomp Launcher — Design

Status: **SHIPPED via recomp-ui** · 2026-07-22

The pre-boot launcher is no longer an in-tree RmlUi UI. Every snesrecomp game
uses the shared Dear ImGui launcher from the `lib/recomp-ui` submodule
([mstan/recomp-ui](https://github.com/mstan/recomp-ui)).

## Integration

```cmake
include(${SNESRECOMP_ROOT}/runner/runner.cmake)
add_executable(MyGame ...)
snesrecomp_enable_launcher_ui(MyGame)   # runner/recomp_ui.cmake
snesrecomp_enable_recomp_net(MyGame)    # optional lobby + delay-sync
```

Host `main()` calls `recomp_launcher_run_window()` behind `#ifdef RECOMP_LAUNCHER`
and supplies game facts through `RecompLauncherCGameInfo` /
`launcher_profile_apply("snes", ...)`. See MetalWarriorsSNESRecomp for a
complete host.

## What stayed in snesrecomp

- `runner/src/launcher.c` / `launcher.h` — ROM resolve, CRC/SHA, `rom.cfg`
  (console-agnostic helpers used when the GUI is skipped with `--no-launcher`)
- Lobby / netplay backends — `snes_lobby_client.*`, `snes_netplay.*`, consumed
  by recomp-ui through host callbacks

## What was removed

- RmlUi + FreeType submodules (`lib/RmlUi`, `lib/freetype`)
- In-tree `runner/src/launcher/launcher_gui.*` and Rml markup/assets
- `tools/build_launcher_deps.ps1`

Historical RmlUi notes below this line are obsolete; keep this file as the
pointer to recomp-ui rather than a second design track.
