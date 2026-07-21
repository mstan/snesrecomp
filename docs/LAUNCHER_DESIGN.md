# snesrecomp Launcher — Design

Status: **Phase 1 + netplay menu wired** (Metal Warriors first) · Branch: `feat/launcher-and-net` · updated 2026-07-19

Offline RmlUi launcher is opt-in per game via `snesrecomp_enable_launcher()`
(`runner/launcher.cmake`). Metal Warriors shows it by default. Netplay menu
flow (home → lobbies → room) and MotK-style WebSocket lobby client are wired;
the in-game `recomp-net` host loop is still a follow-up.

A shared, RmlUi-based pre-boot launcher for every snesrecomp game, modeled on
`psxrecomp/runtime/launcher` and `n64recomp/PokemonStadiumRecomp`. Shown inside
the runner's existing SDL/OpenGL window before the emulator boots; the user picks
a ROM and tunes settings, presses LAUNCH, and the chosen values drive this run
*and* are persisted for the next one.

---

## 1. Goals (from the request)

1. A GUI launcher for snesrecomp games, consistent with the PSX/N64 launchers.
2. A **Settings** menu exposing: ROM select + verify badge · **Widescreen
   (experimental, default OFF)** · Display · Audio · **Controllers** (per-player
   SDL gamepad / keyboard / none, with **per-controller d-pad/stick deadzone**) ·
   **MSU-1** (enable + browse MSU directory).
3. **One exe per game** ("both-in-one") — no more separate "regular" and
   "widescreen" executables.
4. **MSU-1 by default, transparently**: on ROM import, write a patched copy
   `<rom>.msu1.<ext>` next to the original (never touching the original), run from
   that copy, and recompile from the MSU-1-patched ROM going forward. Worst case
   the game just uses its native SPC audio (indistinguishable to the user); best
   case the user drops in an MSU-1 pack and gets streamed audio.

---

## 2. Where it lives

**Shared runner component** under `snesrecomp/runner/src/launcher/` (mirrors
`psxrecomp/runtime/launcher`):

```
runner/src/launcher/
  launcher.cpp        # RmlUi data-model, event handlers, view state
  launcher.h          # snes_launcher::run(window, gl_ctx, io, GameInfo, assets_dir)
  assets/launcher.rml # dashboard + settings views (ported from psx launcher.rml)
  assets/img/...       # logo, cart art, check/verdict icons, pad art, caret
  assets/fonts/...     # Lato (reuse psx fonts)
```

The existing `runner/src/launcher.c` (console ROM resolver: CRC/SHA verify +
`rom.cfg` cache) is **kept and reused** as the verification/patch backend — the
GUI calls into it; it is no longer the user-facing entry point. To avoid a name
clash, the GUI module is C++ in `launcher/` while the resolver stays
`launcher.c`; or rename the resolver to `rom_resolve.c`. (Decision: rename to
`rom_resolve.c` for clarity.)

Each game passes a `GameInfo` (display name, expected hashes, MSU-1 patch handle)
and reuses the same UI — SMW first, then Zelda/MMX/Super Metroid get it for free.

### Build wiring
- **MSBuild** (`src/smw.vcxproj` and peers): add the launcher sources, the RmlUi +
  FreeType deps (NuGet or vendored under `third_party/`), and the assets to the
  staged output. RmlUi renders on the existing GL 3.3 core context (`opengl.c`).
- **CMake** (`runner.cmake` + per-game `CMakeLists.txt`, used for Linux/AppImage):
  add RmlUi/FreeType via FetchContent or system packages.

---

## 3. Boot-sequence refactor (main.c)

Today `main()` resolves the ROM (console picker) *before* SDL/GL exists, then
parses config, then creates the window. The launcher needs a live GL context, so
the order becomes:

```
parse args
anchor cwd to exe dir
ParseConfigFile(config.ini) + config.local.ini      # seed g_config
SDL_Init + create window + create GL 3.3 context     # moved earlier
--- snes_launcher::run(window, gl_ctx, g_config, GameInfo, "launcher") ---
    dashboard: pick/verify ROM -> (MSU-1) produce <rom>.msu1.sfc -> chosen path
    settings:  mutate g_config in place
    LAUNCH  -> persist config.ini + keybinds.ini, return chosen ROM path
    QUIT    -> exit cleanly
load ROM (the launcher-chosen path) -> SnesInit -> game loop
```

The launcher is a pure overlay (does not own the window), exactly like the PSX
one, so a future "reopen settings while running" path can reuse it.

A `--no-launcher` flag (and the existing positional-ROM / `--config` paths) skip
the GUI for headless/CI/debug-server use, falling straight through with
`g_config` as parsed.

---

## 4. Settings model & persistence

**New requirement:** there is no config write-back today (`EnsureConfigIni`
only writes defaults when the file is missing; `ParseConfigFile` is read-only).
The launcher must serialize settings back out.

- `config_write(const char *path)` — serialize `g_config` to `config.ini`
  (section-preserving, comment-preserving where practical). New module
  `runner/src/config_io` shared across games, or a per-game `WriteConfigIni`.
- `keybinds_write(const char *path)` — serialize `KeyBinds` to `keybinds.ini`
  (companion to existing `keybinds_init`).
- On LAUNCH: apply to `g_config`/`KeyBinds` in memory for this run **and** write
  both files so the next run (GUI or `--no-launcher`) sees them.

### Settings views (RmlUi)

**Dashboard:** ROM art + name, verification badge (Header / Hash / Verified, like
the PSX disc panel), Change ROM, MSU-1 status line, LAUNCH.

**Display:** Renderer (OpenGL/SDL/SDL-SW → `output_method`), Window scale & size,
Fullscreen, Aspect ratio (`ignore_aspect_ratio`), Linear filtering, **Widescreen
(EXPERIMENTAL)** + Widescreen HUD.

**Audio:** Frequency, channels, samples, enable, volume; (later) the opt-in
S-DSP cubic shadow + screen LUT flags already in the codebase.

**Controllers** (new sub-menu, one panel per player):
- Player input mode: **SDL gamepad · Keyboard · None** (today gamepad+keyboard can
  both be active; the launcher makes the mode explicit, mapping to
  `enable_gamepad[p]` + `has_keyboard_controls` bit).
- Keyboard remap grid → writes `keybinds.ini [player1]/[player2]`.
- **Per-controller deadzone** (NEW): a slider per player. Requires new config
  fields + plumbing into `HandleGamepadAxisInput` (today the radius deadzone is
  hardcoded at 10000 and the trigger threshold at 12000). Add
  `gamepad_deadzone[2]` (and optionally trigger threshold) to `Config`, read them
  in the axis handler instead of the literals.
- Device→player routing is currently first-come-first-served; surfacing explicit
  device selection is a later enhancement (call it out, don't fake it).

**MSU-1** (see §5): Enable toggle, "Browse MSU directory" (view/place packs),
status (pack detected? track count?).

---

## 5. MSU-1: transparent auto-patch model

### How MSU-1 actually works here (confirmed)
- Hardware ($2000–$2007, PCM streaming, `<base>-<N>.pcm` + `<base>.msu`) is
  **pure runtime** in `runner/src/snes/msu1.c`, enabled today only via env var
  `SNESRECOMP_MSU1`. Default OFF = byte-identical.
- Audio only plays if the **recompiled game code contains the MSU-1 driver** — i.e.
  the game was **recompiled from an MSU-1-patched ROM**. The patch (IPS) injects an
  audio driver into expansion space and hooks the music-play calls; with no pack
  present it falls back to native SPC.
- **Zelda already implements the exact target model**: `tools/apply_msu_patch.py`
  applies `recomp/msu1/alttp_msu.ips` to a throwaway copy at regen, recompiles
  from it, and the launcher accepts **both** the vanilla and patched SHA-256
  (`resolve_rom_sha256_multi`). User only ever supplies the stock ROM.
- **SMW has no MSU-1 IPS patch in-repo.** This is the one real dependency for
  "best case real MSU-1 audio" on SMW (see §7 Risks).

### Build-time (regen)
Adopt Zelda's pattern for every MSU-capable game: regen applies the game's
bundled IPS to a throwaway patched ROM and **recompiles from the patched ROM**.
The shipped exe is therefore MSU-1-capable. Expected hash set becomes
`{vanilla, patched}`.

> Note: recompiling from the patched ROM is **behaviorally** identical with no
> pack (same SPC music) but not strictly byte-identical to a vanilla recompile
> (music calls route through the driver's fallback). The *widescreen-off*
> byte-identical guarantee is unaffected (that is a separate, gated concern). The
> chosen IPS must be a clean fallback patch; audio parity is validated at adoption.

### Runtime (launcher), the requested flow
On ROM import in the dashboard:
1. Verify the imported file against the **vanilla** hash (reuse `launcher.c`).
2. If a bundled IPS exists for this game and `<rom_dir>/<stem>.msu1.<ext>` is
   missing or stale, **apply the IPS to a new file `<stem>.msu1.<ext>`** beside
   the original. The **original is never modified**.
3. Verify the patched copy against the **patched** hash.
4. Use the patched copy as the run ROM; cache its path in `rom.cfg`.
5. Point MSU-1 at the MSU directory (default `<rom_dir>/msu1/` or the
   launcher's "Browse MSU directory" choice); set `msu1` enable + base so
   `msu1_init`/`msu1_set_rom_path` pick it up (replaces the env-var-only flow).
   With no `.pcm` present → native SPC (no user-visible change).

If **no** IPS is bundled for the game, step 2–4 are skipped and the stock ROM is
used as-is (MSU unavailable) — graceful degradation, so the launcher works for
games before their patch is sourced.

New `Config` fields: `bool msu1_enabled; char msu1_dir[...]`. New `GameInfo`
fields: vanilla hash(es), patched hash, IPS patch path/handle.

---

## 6. Single-exe ("both-in-one") shipping

The unified per-game exe is built with **widescreen overrides injected** (the
override branches are gated on `g_ws_active`, byte-identical when off) **and**
**recompiled from the MSU-1-patched ROM** (driver inert without a pack). The
launcher's toggles then select behavior at runtime:
- Widescreen toggle → `config.ini Widescreen=0/1` (default 0, flagged
  experimental).
- MSU-1 → patched-copy + pack directory (default native audio).

`tools/make_release.ps1` collapses from dual-zip to a **single zip** per game.
Validation gate before shipping: frame-diff the unified build with `Widescreen=0`
against the current pristine "standard" build to confirm the WS-off
byte-identical guarantee still holds; confirm audio parity with no MSU pack.

---

## 7. Risks / open items

1. **SMW MSU-1 patch does not exist in-repo.** Sourcing/validating a clean,
   fallback-safe SMW MSU-1 IPS (e.g. a known MIT/community patch) is a
   prerequisite for SMW MSU audio. Until then SMW ships MSU-capable-but-no-patch
   = stock behavior (graceful). Decision needed: source a patch now, or land the
   launcher + widescreen single-exe first and add the SMW patch in a follow-up.
2. **Recompile-from-patched changes the SMW baseline** that the audio-accuracy
   work was validated against — needs an audio-parity pass at adoption.
3. **RmlUi dependency in the MSBuild build** — vendoring vs NuGet; first time
   snesrecomp's Windows build pulls RmlUi + FreeType.
4. **Config/keybinds write-back** is net-new and must round-trip cleanly (don't
   clobber user comments / unknown keys).
5. **Boot-sequence reorder** (GL before ROM resolve) touches every game's main.c;
   keep the `--no-launcher`/positional-ROM/debug-server paths working.

---

## 8. Phased plan

- **P0 — Scaffold:** port RmlUi launcher into `runner/src/launcher/`, wire MSBuild
  + CMake deps + assets, reorder main.c boot, minimal dashboard (ROM select/verify
  + LAUNCH) replacing the console resolver. Build + run SMW.
- **P1 — Settings + persistence:** Display/Audio panels, `config_write`,
  Widescreen toggle (experimental). 
- **P2 — Controllers:** per-player mode, keyboard remap (`keybinds_write`),
  per-controller deadzone (new config fields + axis-handler plumb).
- **P3 — MSU-1:** runtime auto-patch (`<rom>.msu1.<ext>`), MSU-dir browse, enable
  toggle; adopt regen-from-patched for SMW once an IPS is sourced; `{vanilla,
  patched}` hash set.
- **P4 — Single exe:** unify the build (WS-injected + MSU-patched gen), collapse
  `make_release.ps1` to one zip, run the byte-identical/audio-parity gates.
- **P5 — Fan-out:** Zelda/MMX/Super Metroid adopt the shared launcher + GameInfo.
- **P6 — Netplay menu + lobby client (done):** PSX-style flow
  (`home` → Offline/Netplay → dashboard → `netplay_lobbies` → `netplay_room`)
  keeping SNES purple chrome; `runner/src/lobby/snes_lobby_*` talks MotK WS to
  `recomp-net-server` (`ws://netplay.technicallycomputers.ca:8765` /
  `SNES_NET_LOBBY_URL`); launcher
  exits with `SnesNetplayLaunch` handoff (session_id, slot, bind/peer, transport)
  and keeps the lobby WebSocket across Launch for ICE signal relay / rematch.
- **P7 — Netplay host loop (done):** `runner/src/netplay/snes_netplay.*` facade
  over `rnet_session_*` (LAN or ICE); MW enables `snesrecomp_enable_recomp_net` +
  `SNESRECOMP_NET_ICE`; main loop admit/stall + pad publish; lobby `op:signal`
  for ICE. Override with `SNES_NET_TRANSPORT=lan|ice`.
