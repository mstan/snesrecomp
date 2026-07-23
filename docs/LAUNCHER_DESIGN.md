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

Online lobby handoff: recomp-ui prepares `guest_bind` (prefer UDP 7778) before
`join()`; the host passes it through to `snes_lobby_join(..., guest_bind)`
(engine still normalizes NULL/empty as a fallback) and
`snes_lobby_try_fill_launch()` from `fill_launch`. See
`docs/RECOMP_NET.md` → "Lobby join / launch handoff".

Soft-return rematch (peer quit / Escape → waiting room → Play again): call
`snes_host_ensure_sdl()` + `snes_host_session_reset()` at `session_reboot`,
and `snes_netplay_soft_exit_to_lobby()` on peer leave. Checklist:
`docs/RECOMP_NET.md` → "Soft-return rematch checklist".

**Policy:** netplay + launcher interaction fixes belong in snesrecomp or
recomp-ui so every title benefits. Do not grow game `main.c` with shared
networking UX. Per-title sticky state uses `RtlGameInfo.session_reset` (and
related hooks) — see `docs/RECOMP_NET.md` → "Layering policy".

## What stayed in snesrecomp

- `runner/src/launcher.c` / `launcher.h` — ROM resolve, CRC/SHA, `rom.cfg`
  (console-agnostic helpers used when the GUI is skipped with `--no-launcher`)
- Lobby / netplay backends — `snes_lobby_client.*`, `snes_netplay.*`,
  `snes_host_session.*`, consumed by recomp-ui through host callbacks
  (`snesrecomp_enable_recomp_net`)
- `lib/recomp-net` — delay-sync / ICE transport submodule

## What was removed

- Legacy GUI + FreeType submodules
- In-tree `runner/src/launcher/launcher_gui.*` and legacy markup/assets
- `tools/build_launcher_deps.ps1`
- `lib/recomp-ui` submodule and `runner/recomp_ui.cmake` (games own the UI pin)
