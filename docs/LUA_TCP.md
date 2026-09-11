# Lua over TCP spike

An opt-in Lua 5.4.9 interpreter exposes a useful subset of the
[BizHawk Lua API](https://tasvideos.org/Bizhawk/LuaFunctions). This is a developer
experiment, not a complete EmuHawk replacement. The first host integration and
gameplay examples live in SuperMarioWorldRecomp, `tools/lua/`.

Build the game with `-DSNESRECOMP_ENABLE_LUA=ON`, then launch with
`SNESRECOMP_LUA_PORT=4380`. `SNESRECOMP_LUA_PAUSED=1` starts at frame zero.
Both the build option and runtime port are required. Ordinary builds do not
download/link Lua or open this socket. Lua's source archive and SHA-256 are
pinned in `runner/lua.cmake`; its MIT license is in the downloaded source.

## Client

From this repository, with a game listening on port 4380:

```powershell
python tools/lua_tcp.py eval 'return emu.framecount(),mainmemory.read_u8(0x19)'
python tools/lua_tcp.py eval 'mainmemory.write_u8(0x19,3)'
python tools/lua_tcp.py load path/to/helpers.lua
python tools/lua_tcp.py run path/to/frame_loop.lua
python tools/lua_tcp.py pause
python tools/lua_tcp.py step 60
python tools/lua_tcp.py resume
python tools/lua_tcp.py reset
```

Use `--port N` before the subcommand to select another port. `LuaClient` in
`tools/lua_tcp.py` also provides a persistent Python context manager.

## Supported compatibility subset

| Library | Implemented functions |
| --- | --- |
| `memory` | `read_u8`, `read_s8`, `write_u8`, `write_s8`; signed/unsigned 16/24/32-bit reads/writes with `_le` and `_be`; `readbyte`, `writebyte`, `readbyterange`, `read_bytes_as_array`; `getmemorydomainlist`, `usememorydomain`, `getcurrentmemorydomain`, `getmemorydomainsize` |
| `mainmemory` | Same reads/writes/ranges, always WRAM; `getsize` |
| `joypad` | `set(buttons[, controller])`, `get([controller])` |
| `event` | `onframestart`, `onframeend`, `unregisterbyid`, `unregisterbyname` |
| `emu` | `framecount`, `getsystemid`, `frameadvance` |
| `client` | `pause`, `unpause`, `ispaused` |
| `console` | `log`; global `print` also captures output |

`WRAM` offsets span 0..0x1ffff. `CARTROM` is the host-provided, read-only ROM
image, indexed by file offset. `System Bus` supports only banks $7E/$7F and
their low-bank WRAM mirrors. Other bus addresses, unknown domains and ranges
crossing a domain/mirror boundary raise errors. No MMIO side effects, ROM
patching, SRAM, VRAM, CPU-register or instruction-hook API is implemented.
`readbyterange` is zero-indexed; `read_bytes_as_array` is one-indexed. Reads
are capped at 4096 bytes per range call.

Controller numbers are 1 and 2, with `B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R`.
When the controller is omitted, keys are prefixed, e.g. `"P1 Right"`.
Overrides last one simulated frame; omitted buttons retain host input, while
`false` explicitly releases a button. Repeat `joypad.set` in a frame callback
or loop to hold a button. Input issued after a frame ends targets the next frame.

```lua
event.onframestart(function()
    mainmemory.write_u8(0x1497, 2) -- SMW invulnerability timer
end, "invincible")

-- Send with `run`, which owns one resumable script coroutine:
for i = 1, 120 do
    joypad.set({Right=true}, 1)
    emu.frameadvance()
end
```

Frame callbacks have optional names and return registration IDs. Up to 64
callbacks can be registered. A failing callback is removed and its error is
available in `status`. Registrations created during a callback dispatch begin
on a subsequent dispatch. `emu.frameadvance` works in a `run` script, not
inside `eval` or a callback. Each yield permits one frame even while Lua is
paused. The frame counter measures completed host frames since bridge startup;
it does not rewind with the game's savestates.

GUI drawing, savestates, movie/TAS APIs, `comm.*`, `emu.yield`, and other
unlisted BizHawk APIs are not implemented. Lua base/table/string/math/utf8
libraries are available, but `os`, `io`, `package`, `debug`, `coroutine`,
`dofile`, `loadfile`, `pcall`, and `xpcall` are omitted. This trusted local
developer interface is not a security sandbox. An instruction hook limits
each evaluation/callback/coroutine resume to 100,000 Lua instructions, and
the VM allocator caps live memory at 16 MiB. C library calls do not have a
wall-clock deadline. Memory writes before an error are not rolled back.

## Wire protocol and lifetime

One command per LF-terminated ASCII line; responses are one JSON line.
`eval HEX` executes UTF-8 Lua source encoded as hexadecimal, allowing scripts
to contain newlines without ambiguity. `run HEX` replaces the running
coroutine, while preserving globals and event registrations. Source must be
text; wire lines must be shorter than 131072 bytes including the newline.

Other commands: `ping`, `status`, `pause`, `resume`, `step N` (1..100000),
`stop` (coroutine and pending input only), `reset` (new VM, removes globals,
callbacks and pending input; preserves game RAM, frame count and pause state).
For removing callbacks without resetting globals, use event unregistration.
`step` acknowledges scheduling; wait for `status.frame` to reach the target
or use Python's `LuaClient.step`. A running coroutine/callback can deliberately
change pause/step behavior, so stop it before independent exact stepping.

```json
{"ok":true,"frame":123,"paused":true,"running":false,"values":["3"],"output":"","error":""}
```

Up to 16 return values are reported: booleans/null are JSON primitives;
numbers are strings to preserve 64-bit integers; strings are limited to 1024
bytes each; other values become type names (e.g. `"table"`). Inspect a table
in Lua or format its contents. Binary/high bytes in strings are escaped as
`\u00xx`, preserving bytes rather than decoding UTF-8. Console output has a
4095-byte buffer, drained by each response. `status` retains the last
asynchronous error; other commands clear it before execution.

The nonblocking server binds only 127.0.0.1 and accepts one client at a time.
Extra clients are closed. Disconnecting leaves scripts and pause state intact
so short-lived CLI commands compose; use `reset` to stop all automation.
Partial reads/writes and pipelined commands are handled without blocking the
game. A full input buffer without LF drops the connection.

## Host integration

Call `lua_bridge_init(wram, size, rom, rom_size, port)` after initializing
memory. Each desktop-loop iteration calls `lua_bridge_poll`, including while
paused. Admit no new frame while `lua_bridge_paused()` is true. Wrap each
actual game frame with `inputs = lua_bridge_frame_start(inputs)` and
`lua_bridge_frame_end()`, and call shutdown on exit. These calls run on the
game thread, outside the CPU/frame runner; the socket has no worker thread.
Input bits use the runner's 12 bits per player plus controller-present bits.

Games may register `lua_bridge_set_game_command_handler(handler)` after init.
The host extension `game.command(name, args)` dispatches strings to that handler
on the game thread and returns its result string, or raises a Lua error on
failure. This is not a BizHawk API. The handler also receives `__reset` on TCP
VM reset and shutdown to clear its automation. SMW uses it for a separate
fireball pool (`fire_stream`, `fire_stream_status`, `fire_stream_reset`), with
Lua helpers for continuous emission and holding the normal fire buttons.

The SMW spike supports stock single-player and rejects co-op builds. Use Lua's
pause controls: a separate debugger breakpoint or the host pause key can still
prevent frames from advancing. Lua state is not included in game saves or
netplay. SMW validates the integration with `tools/lua/validate.py`, including
real game navigation, spawning, projectile motion and cadence measurements.

The ROM-free TCP regression harness is built with
`cmake -S tests/lua -B build/lua-on -DSNESRECOMP_ENABLE_LUA=ON`, followed by
`cmake --build build/lua-on`. Run `python tests/lua/test_bridge.py` with the
resulting `lua_bridge_host` executable path. CI runs it on Windows, Linux and
macOS, and separately builds with Lua disabled.
