# recomp-net (netcode)

snesrecomp vendors [recomp-net](https://github.com/TechnicallyComputers/recomp-net)
as a git submodule at `lib/recomp-net`. Any game that builds with
`runner/runner.cmake` can link the library and drive delay-sync multiplayer
from the game main loop.

recomp-net is **opt-in**. Shipping single-player builds are unchanged unless
the game enables it.

## Init the submodule

```sh
git submodule update --init --recursive lib/recomp-net
# or, for a fresh clone of snesrecomp:
git submodule update --init --recursive
```

## Enable from a game `CMakeLists.txt`

After `include(.../runner/runner.cmake)` and `add_executable(...)`:

```cmake
snesrecomp_enable_recomp_net(MyGame)
```

That `add_subdirectory`s `lib/recomp-net` once (examples/tests off) and links
`recomp_net` into the game. It also defines `SNESRECOMP_NET=1` on the target.

### Alternative: cache option

```sh
cmake -S . -B build -DSNESRECOMP_ENABLE_NET=ON
```

Then link the shared runner libs (include `recomp_net` when the option is on):

```cmake
target_link_libraries(MyGame PRIVATE
    ${SNESRECOMP_RUNNER_LIBRARIES}
    # ... SDL2, OpenGL, etc.
)
```

### ICE / WAN (optional)

LAN UDP works with the default build. For NAT traversal via libjuice:

```sh
cmake -S . -B build -DSNESRECOMP_NET_ICE=ON
# with snesrecomp_enable_recomp_net, set the cache before the helper:
#   set(SNESRECOMP_NET_ICE ON CACHE BOOL "" FORCE)
```

If libjuice is not installed or vendored, recomp-net may FetchContent it
(network required at configure time). See `lib/recomp-net/README.md`.

## Host loop (game side)

Preferred: use the SNES facade (`runner/src/netplay/snes_netplay.h`) which
`snesrecomp_enable_recomp_net` compiles into the game:

```c
#include "snes_netplay.h"

SnesNetplayConfig cfg;
snes_netplay_config_defaults(&cfg);
cfg.enabled = 1;
cfg.local_slot = launch.local_slot;
cfg.input_player = -1; /* auto — see ResolveNetplayInputPlayer in game main.c */
cfg.session_id = launch.session_id;
cfg.transport = launch.transport; /* 0 auto, 1 ICE, 2 LAN */
strncpy(cfg.bind_hostport, launch.bind_hostport, sizeof(cfg.bind_hostport) - 1);
strncpy(cfg.peer_hostport, launch.peer_hostport, sizeof(cfg.peer_hostport) - 1);
/* Resolve auto → 0/1, then start. Sample host device input_player (not slot).
 * Auto must ignore offline P2-only pad assignment so a remote guest keyboard
 * (device 0) still wraps into sim P2; host with a sole pad on P2 samples 1. */
snes_netplay_start(&cfg);

for (;;) {
    /* Stage exclusive local P1 (or input_player device); library maps → local_slot. */
    if (snes_netplay_needs_local_sample())
        snes_netplay_stage_local(local_device_buttons_12bit);
    if (!snes_netplay_poll_admit())
        continue; /* stall until INPUT_CONFIRM hash agrees — do not RtlRunFrame */
    RtlRunFrame(snes_netplay_published_inputs() | snes_netplay_active_mask());
    snes_netplay_finish_frame();
}
```

Transport selection (`cfg.transport` / `SNES_NET_TRANSPORT`):

- **LAN** — `rnet_session_start_lan` using lobby-rewritten bind/peer hostports.
- **ICE** — `rnet_session_start_ice` + MotK lobby `op:signal` relay
  (`snes_lobby_send_signal` / `snes_lobby_poll_signal`). Requires
  `SNESRECOMP_NET_ICE=ON` and a live lobby WebSocket (launcher keeps it across
  Launch). Auto picks ICE when the peer hostport is not private/loopback.

Rules that matter for SNES recomp hosts:

- Use `publish` / `snes_netplay_published_inputs` as the **only** pad source for
  locked ticks; do not let local-only controller reads enter the shared sim.
- Keep RNG / timers / frame pacing deterministic across peers. Pad blob bytes
  `[2..3]` carry host DP `$1A/$1B` (applied on admit) so Metal Warriors
  SCRAMBLE / NEW BATTLEFIELD rolls stay host-authoritative.
- Prefer one thread owning `pump` + sim advance (API is not internally locked).
- While `snes_netplay_active()`, the pre-frame wall-clock SPC catch-up is
  disabled. Audio stays on the runner's normal deterministic guest-frame/APU
  coupling, and the audio callback remains a consumer only; it never advances
  emulation.
- Metal Warriors netplay H2H: when dual-viewport WRAM `$1EB2` is set, each peer
  presents only its local half (slot 0 = top / P1, slot 1 = bottom / P2),
  scaled to the full window. Sim still renders the full split — present-only,
  so determinism is unchanged. Opt out with `SNESRECOMP_MW_H2H_LOCAL_VIEW=0`.
- Metal Warriors H2H present: full-frame local defaults **ON** for netplay.
  Offline uses native dual split (no local-full present). OAM `+$78` /
  vert-widen defaults **ON** for netplay (`SNESRECOMP_MW_H2H_VERT_WIDEN=0`
  to opt out); active-list Y `#$A8`→`#$E0` so tall sprites slide off instead
  of popping when the anchor crosses 168. Spawn-Y widen stays opt-in
  (`SNESRECOMP_MW_H2H_SPAWN_Y_WIDEN=1`). Both hard-off offline. BG2 stripe
  row widen (default 12 / `SNESRECOMP_MW_BG2_ROWS`) is **netplay or
  widescreen-expand only** — offline dual keeps native 8 rows so the HDMA
  center seam HUD (direction arrows) is not stomped. Keep
  `OAM_CULL` on (disabling corrupts isolated 1P/2P views). End-match results
  OAM is owned by **OBJ priority 3** (`a34`/`a35` in dumps): when ≥8 such
  sprites exist across cam buffers, present groups each cam's UI and pins by
  mean-X (native left/wins → top, right/menu → bottom; no gameplay `y_shift`,
  never cam-delta reproject). Glyph X-span split is wrong here — both
  clusters are center-stacked. Mutual raw-XY is **not** used. Opt out
  full-frame: `SNESRECOMP_MW_H2H_FULL_FRAME=0`.
- Elevator / platform probe: `SNESRECOMP_MW_ELEV=1` logs `[mw_elev]` — walks
  the `$1E14` object list (flags/X/heuristic Y + 0x20 raw bytes) plus BG1 `$7F`
  tile-patch counters, BG2 ROM-idle mask, and OAM occupancy. Flags `vw` /
  `syw` show OAM vert-widen vs spawn-Y widen. Elevator fingerprint:
  `+$08=$D5B8`, family `+$0A=$00B1`.
- End-match / results OAM probe: `SNESRECOMP_MW_RESULTS=1` logs `[mw_results]`
  (~2 Hz, dual only). Adds `ui_prio3` count alongside mutual/reproject stats.
- Metal Warriors netplay H2H Phase 2a (shared horizontal widescreen): netplay
  sessions **force** `g_ws_extra = 71` on every peer. Offline hard-disables
  widescreen for traditional split-screen local multiplayer. Lobby
  `match_caps` carry 71; the launcher hides the Widescreen toggle.
- Lobby `match_caps` (host-authoritative): create/start carry
  `{widescreen,widescreen_hud,ignore_aspect,input_delay,ws_extra}`; guests
  apply on launch. See `recomp-net-server/docs/WS_LOBBY.md`.
- Metal Warriors soft-return rematch: `MwSessionReset()` clears the LLE resume
  latch (`s_lle_did_reset` / resume PC / `g_cpu`) before `SnesInit`. Without
  that, rematch resumes a stale WAI on a wiped chip (`nmiEn=0`, blank).
  Autosave load/save is skipped around lobby rematch so peers cold-boot alike.
- Metal Warriors H2H Phase 2b (full-frame local): default ON for present.
  - **Present:** rebuild BG1 strips from `$7F` for the **local** camera into
    VRAM (save/restore so sim stays dual-deterministic). Present scrolls are
    forced to that camera (P1: `$1E16`/`$1E18`; P2: `$1E1A`/`$1E1C`). Stable
    half→full Y: OAM=BG `base+8`; present undoes unified emit `+$78`. Dual HDMA
    skipped. BG1 rebuild keys `$7F` from `$7E:42B3` at the **local** present
    cam (not dual sticky/`$1E36`, which is one shared P1-oriented strip and
    pinned both peers' floors to P1 with violent shake). Sticky/live is
    fallback only when 42B3 is unset. Opt out with
    `SNESRECOMP_MW_H2H_BG1_REBUILD=0` (regressive). BG2 `$7F` rebuild only when
    streaming; narrow idle BG2 (elevators) uses the 1P `retainHistory` +
    west-ROM path and tracks the local camera when dual BG2 WRAM mirrors are
    `$0`.
    OAM **vert-widen** (default ON; `VERT_WIDEN=0` opts out): ROM-pokes
    drawer Y `CMP` (`#$68`/`#$70`→`#$E0`, `#$FFF1`→`#$FF70`), active-list
    `#$A8`→`#$E0`, + staging `+$78` bias / cam-capture. Stage-prop anchors
    at sy≈225 are forced onto the list via pre-CMP hooks at `$809280`/
    `$8092A0` (prop-only — global `#$0100`/`#$0140` leaked P2-floor into
    cam0). Present uses local cam-capture + reprojected other-cam buffer
    (half-cull only when vert-widen is off). Capture buffer tag follows
    drawer `ADC $86/$88` (cam0) vs `ADC $8A/$8C` (cam1). Family-`$00B1`
    **mover** metas only (`$C382`/`$C39E`/`$C6A4`/`$C400`/`$C5F2`/
    `$C3EC`/`$C3C4` — not every `$00B1`≠`$D5B8`; pickups like `$9FE2`
    stay shared and are **mirrored into both cam-capture buffers** at
    commit so dual-drawer OAM pressure cannot leave a half-culled
    sprite): latch at `STA $86` /
    dual `$8087xx`; `$80882F` (pre-`TAX`) reinforces only (never clears).
    Commit recovers list object via `$96`→`$136E,Y` when tile emit
    clobbered X. Cam rebucket / hi-byte owner force **converts** `sx/sy`
    (`sy' = sy + oldCamY − newCamY`). World Y = `dest_cam+sy'` (not
    `+$04`). Object `+$06` dual-slot is **hi-byte only** (`$01xx`→P1,
    `$02xx`→P2); low-byte `$0002/$0004/$0006/$0008` on `$C382` are not
    owners. Home = hi-byte `+$06`, else nearer dual-slot mech each frame
    (4× hysteresis when both mechs exist). Unassigned (−1) until a mech
    exists. Present OAM: home peer only; X = live ± commit meta. Y =
    live+`y_shift` only (BG1-aligned — never drop `y_shift`). With
    vert-widen ON, half→full `y_bg`/`y_oam` is **0** (VW already fills
    ~224 vs `cam_raw`); a +64 recenter parked dual-bottom movers at
    `ny≥224` (brown off-strip, stripe skip_y or ~64px sky gap). Sticky
    last tile covers capture misses (sticky may default `meta_oy=−10`
    when convert miss; present does **not** force `moy==0→−10`). No
    `$7F` tile-grid OAM snap. Non-home movers blanked from local BG1 at
    live + previous trail after rebuild and again after margin prefill.
    Full-frame BG1 never falls back to `$1E36`. `$C382` = 1 OAM tile +
    BG1 body. Do **not** XY-cull gameplay/reproject near `$00B1` props
    (mechs on `$C6A4`/`$C382` vanished; A/B `OAM_CULL=0`). Commit
    recovers movers by `$82` meta match only (no loose screen-XY
    rebind). `SNESRECOMP_MW_ELEV=1`: `prop_lo`/`cap`, `bg_dy`, `skip_y`.
    Present-only; no sim bbox / `$7F` gate. Dual staging wrap at
    `CPX #$0200` needs vert-widen (`SNESRECOMP_MW_H2H_OAM_WRAP=0`
    disables). **Spawn-Y widen**
    (opt-in `SPAWN_Y_WIDEN=1`): `$82F709`/`$82F721`/`$82F733` top window +
    `$8283AC` radius +160. Default **OFF** — playtest regressed object
    lifetime and did not spawn `$D5B8` elevators. Needs
    `kInterpPreOpcodeHookSlots` ≥ ~160 (runner default 192).
    `SNESRECOMP_MW_H2H_OAM_CULL=0` disables capture present/cull. Guest 1P
    object drawer remains opt-in only (`SNESRECOMP_MW_H2H_OBJ_OAM=1`).
  - **Sim taller HDMA/stripe** (`SNESRECOMP_MW_H2H_TALLER=1`) default
    **OFF** — breaks dual split / shared strip.
  - **Top bar HUD** (default ON with full-frame): present paints a solid
    16px bar at the top of each local view (masks the top FOV transition)
    with an opponent-direction marker from dual cams. Replaces the native
    dual-seam strip that full-frame skips. Opt out:
    `SNESRECOMP_MW_H2H_TOP_BAR=0`.
  - Opt out present: `SNESRECOMP_MW_H2H_FULL_FRAME=0` (half-crop),
    `SNESRECOMP_MW_H2H_LOCAL_VIEW=0` (show split).
- Savestate / SRAM during netplay is **host-only** (`local_slot == 0`):
  - **Host** keeps personal files under `saves/` (continuous). Save/load and
    SRAM flush apply **immediately** on the host; the blob is then shipped
    async to the guest (`STATE_*` chunks). Load/SRAM stall admit until the
    guest catches up; save does not stall the sim.
  - **Guest** redirects `RtlSaveRoot()` to `saves/netplay/` so host-driven
    mirrors never overwrite personal `saves/save.srm` / `saveN.sav`. On
    session end the sandbox is flushed and the personal root is restored.
  - On match start the host syncs live battery SRAM so both peers share the
    same cart RAM for the session.

Low-level API: `lib/recomp-net/docs/host_integration.md`.

## What snesrecomp does vs. what the game does

| Layer                                                                          | Responsibility                                                                 |
| ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------ |
| snesrecomp (`lib/recomp-net`, `snes_netplay`, lobby client)                    | Vendors netcode, pad/admit facade, MotK WS + ICE signal relay                  |
| Game runtime                                                                   | Launcher handoff → `snes_netplay_start`, gate `RtlRunFrame`, sample local pads |
| [recomp-net-server](https://github.com/TechnicallyComputers/recomp-net-server) | Lobby membership, launch, ICE signal relay                                     |

## Windows MSBuild / `lib/` superbuild

CMake game builds should use `snesrecomp_enable_recomp_net` (above).

To also build `recomp_net.lib` from the launcher dep superbuild:

```sh
cmake -S lib -B lib/_build -DSNESRECOMP_BUILD_RECOMP_NET=ON
cmake --build lib/_build --target recomp_net
```

Hand-maintained `.vcxproj` games must add the include path
`snesrecomp/lib/recomp-net/include` and link the resulting static library
themselves (same pattern as RmlUi/FreeType).
