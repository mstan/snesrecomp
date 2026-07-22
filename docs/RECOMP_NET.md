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
  Offline uses native dual split (no local-full present). Opt out:
  `SNESRECOMP_MW_H2H_FULL_FRAME=0` (legacy half-crop).
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
  - **Present:** rebuild BG1/BG2 strips from `$7F` for the **local** camera
    into VRAM (save/restore so sim stays dual-deterministic). Present
    scrolls are forced to that camera (P1: `$1E16`/`$1E18`; P2: `$1E1A`/
    `$1E1C`).     Stable half→full Y: OAM=BG `base+8`; present undoes unified emit
    `+$78` on both slots (P2 ROM `ADC`; P1 STA hook) so high tiles stay in
    unsigned OAM Y.     Dual HDMA skipped. BG2 `$7F` rebuild only when streaming;
    narrow idle BG2 (elevators) uses the 1P `retainHistory` + west-ROM path and
    tracks the local camera when dual BG2 WRAM mirrors are `$0`.
    OAM: **vert-widen** ROM-pokes drawer Y `CMP` (`#$68`/`#$70`→`#$E0`,
    `#$FFF1`→`#$FF70`); active-list `#$A8` stays native (widening it listed
    same-floor far objects into cam1 and sliced P1). Present uses the local
    cam-capture buffer plus a **reprojected** pass of the other cam's
    buffer (fixes small props dropped when dual P1-half fills OAM before
    the P2-half emit).
    Dual tile×2 staging wrap at `CPX #$0200` (`SNESRECOMP_MW_H2H_OAM_WRAP=0`
    disables).
    Needs `kInterpPreOpcodeHookSlots` ≥ ~130 (runner default 192).
    `SNESRECOMP_MW_H2H_VERT_WIDEN=0` disables.
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

| Layer | Responsibility |
|-------|----------------|
| snesrecomp (`lib/recomp-net`, `snes_netplay`, lobby client) | Vendors netcode, pad/admit facade, MotK WS + ICE signal relay |
| Game runtime | Launcher handoff → `snes_netplay_start`, gate `RtlRunFrame`, sample local pads |
| [recomp-net-server](https://github.com/TechnicallyComputers/recomp-net-server) | Lobby membership, launch, ICE signal relay |

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
