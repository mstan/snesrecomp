# snesrecomp Launcher — Design

Status: **SHIPPED via recomp-ui** · 2026-07-23

The pre-boot launcher is not part of the snesrecomp engine tree. Games that
need the shared Dear ImGui launcher vendor
[mstan/recomp-ui](https://github.com/mstan/recomp-ui) as a **repo-root**
submodule and wire it themselves. snesrecomp keeps only recomp-net (and the
lobby / netplay host facades that recomp-ui drives through callbacks).

## Integration

```cmake
# In the game repo (not snesrecomp):
#   git submodule add https://github.com/mstan/recomp-ui.git recomp-ui

include(${SNESRECOMP_ROOT}/runner/runner.cmake)
add_executable(MyGame ...)

set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/recomp-ui" CACHE PATH "" FORCE)
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
recomp_target_launcher_ui(MyGame)

snesrecomp_enable_recomp_net(MyGame)    # lobby + delay-sync (engine)
```

Host `main()` calls `recomp_launcher_run_window()` behind `#ifdef RECOMP_LAUNCHER`
and supplies game facts through `RecompLauncherCGameInfo` /
`launcher_profile_apply("snes", ...)`. See MetalWarriorsSNESRecomp for a
complete host.

Online lobby handoff: `snes_lobby_join(..., NULL)` (engine picks a free guest
UDP port) and `snes_lobby_try_fill_launch()` from `fill_launch`. See
`docs/RECOMP_NET.md` → "Lobby join / launch handoff".

## What stayed in snesrecomp

- `runner/src/launcher.c` / `launcher.h` — ROM resolve, CRC/SHA, `rom.cfg`
  (console-agnostic helpers used when the GUI is skipped with `--no-launcher`)
- Lobby / netplay backends — `snes_lobby_client.*`, `snes_netplay.*`, consumed
  by recomp-ui through host callbacks (`snesrecomp_enable_recomp_net`)
- `lib/recomp-net` — delay-sync / ICE transport submodule

## What was removed

- RmlUi + FreeType submodules (`lib/RmlUi`, `lib/freetype`)
- In-tree `runner/src/launcher/launcher_gui.*` and Rml markup/assets
- `tools/build_launcher_deps.ps1`
- `lib/recomp-ui` submodule and `runner/recomp_ui.cmake` (games own the UI pin)
