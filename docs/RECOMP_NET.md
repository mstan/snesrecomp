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

```c
#include "recomp_net/recomp_net.h"

/* Implement RNetHostVTable: sample_local, publish, optional on_signal/now_ms */
RNetSession *s = rnet_session_create(&cfg, &host);
rnet_session_start_lan(s, "0.0.0.0:7777", "127.0.0.1:7778");

for (;;) {
    rnet_session_pump(s);
    if (!rnet_session_is_running(s))
        continue;
    if (rnet_session_try_admit(s, rnet_session_sim_tick(s))) {
        /* Apply published pads, then one authoritative frame (e.g. RtlRunFrame). */
        rnet_session_advance(s);
    }
    /* else: stall — do not advance the shared sim */
}
```

Rules that matter for SNES recomp hosts:

- Use `publish` as the **only** pad source for locked ticks; do not let
  local-only controller reads enter the shared sim.
- Keep RNG / timers / frame pacing deterministic across peers.
- Prefer one thread owning `pump` + sim advance (API is not internally locked).

Full details: `lib/recomp-net/docs/host_integration.md`.

## What snesrecomp does vs. what the game does

| Layer | Responsibility |
|-------|----------------|
| snesrecomp (`lib/recomp-net`, `runner/recomp_net.cmake`) | Vendors sources, CMake target, link helper |
| Game runtime | Lobby/config negotiation, `RNetHostVTable`, input merge, when to call `RtlRunFrame` / stall |
| [recomp-net-server](https://github.com/TechnicallyComputers/recomp-net-server) (optional) | Out-of-band lobby / ICE signaling control plane |

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
