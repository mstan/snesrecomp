"""ROM-free TCP integration checks; pass the built lua_bridge_host executable."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from lua_tcp import LuaClient

exe = str(Path(sys.argv[1]).resolve())
subprocess.run([exe, "0"], check=True)  # Runtime opt-out is inert.
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
process = subprocess.Popen([exe, str(port)], env=dict(os.environ, SNESRECOMP_LUA_PAUSED="1"))
try:
    for _ in range(100):
        try:
            client = LuaClient(port=port)
            break
        except OSError:
            if process.poll() is not None: raise RuntimeError("host exited")
            time.sleep(.05)
    else: raise TimeoutError("host did not listen")
    with client as c:
        def values(source): return c.eval(source)["values"]
        def fails(source, message):
            try: c.eval(source)
            except RuntimeError as error: assert message in str(error), error
            else: raise AssertionError(source)
        assert c.command("status")["frame"] == 0
        assert values("return game.command('test.echo','hello')") == ["hello"]
        fails("game.command('bad')", "unknown test command")
        assert values("memory.write_u32_le(0x120,0x89abcdef); return memory.read_u32_be(0x120),memory.read_s8(0x120)") == ["4023233417", "-17"]
        fails("memory.write_u8(0,1,'CARTROM')", "read-only")
        fails("memory.read_u16_le(0x1ffff)", "outside WRAM")
        fails("while true do end", "budget exceeded")
        fails("string.rep('x',32*1024*1024)", "not enough memory")
        assert values("return true,false,nil,string.char(0,10,34,92,255)") == [True, False, None, '\x00\n"\\\xff']
        wire = b"eval " + b"return 42".hex().encode() + b"\n"
        c.socket.sendall(wire[:7]); time.sleep(.01)
        c.socket.sendall(wire[7:]+b"ping\n")
        assert json.loads(c.reader.readline())["values"] == ["42"]
        assert json.loads(c.reader.readline())["ok"]
        c.eval("event.onframestart(function() joypad.set({Y=true},1) end,'held')")
        c.step(3)
        assert values("return mainmemory.read_u32_le(0x100),joypad.get(1).Y") == [str((1 << 30) | 2), True]
        c.command("reset")
        assert values("return joypad.get(1).Y,game.command('test.resets')") == [False, "1"]
        c.step(1)
        assert values("return mainmemory.read_u32_le(0x100)") == ["0"]
        c.run("for i=1,4 do emu.frameadvance() end; completed=true")
        for _ in range(100):
            if not c.command("status")["running"]: break
            time.sleep(.01)
        assert values("return completed,emu.framecount()") == [True, "8"]
        c.eval("event.onframeend(function() error('callback failed') end,'bad')")
        assert "callback failed" in c.step(1)["error"]
        assert values("return event.unregisterbyname('bad')") == [False]
    print("PASS: Lua TCP framing, memory, limits, callbacks, input/reset and host commands")
finally:
    process.terminate()
    process.wait(timeout=5)
