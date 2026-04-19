"""L3 execution harness: per-function behavioral equivalence testing.

Given a savestate S and a function F, this harness:
  1. Launches both smw.exe binaries (recomp on 4377, oracle interpreter on 4378)
  2. Loads S into both
  3. Invokes F on each side (recomp: direct C call via registry; oracle:
     set PB:PC and run interpreter until a sentinel return address)
  4. Captures post-invoke state on both sides
  5. Diffs WRAM + VRAM and returns a LuaResult-style report

When run directly as __main__, each L3 test file exercises harness.run_func
with a name, entry PC, return type, and fixture path; then asserts on the
resulting diff.

Per-game pieces (fixture files, which function to test, expected result
shape) live in each game's test file. harness.py itself is game-agnostic.
"""
import functools
import json
import os
import pathlib
import socket
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple


# Note: do NOT call .resolve() — `snesrecomp` is a junction inside the
# SMW repo and resolving follows it out, breaking the parent chain.
REPO = pathlib.Path(__file__).parent.parent.parent.parent
# Locate both binaries. Tests error out immediately if either is missing.
RECOMP_EXE = REPO / 'build' / 'bin-x64-Release' / 'smw.exe'
ORACLE_EXE = REPO.parent / 'SuperMarioWorldRecomp-oracle' / 'build' / 'bin-x64-Release' / 'smw.exe'
FIXTURES_DIR = REPO / 'snesrecomp' / 'tests' / 'l3' / 'fixtures'

RECOMP_PORT = 4377
ORACLE_PORT = 4378

# Regions we diff after an invoke. Each is (name, ram_addr_or_reader,
# length). 'read' form pulls from the debug server directly.
DIFF_REGIONS = [
    ('wram', 0x0000, 0x20000),
]
# VRAM / CGRAM / OAM read via dedicated TCP commands — handled in snapshot().


# ---- process management ---------------------------------------------------

def _kill_existing():
    try:
        subprocess.run(
            ['taskkill', '/F', '/IM', 'smw.exe'],
            capture_output=True, check=False,
        )
    except FileNotFoundError:
        pass


def launch_pair():
    """Launch both smw.exe instances paused. Returns when both TCP ports listen."""
    if not RECOMP_EXE.exists():
        raise FileNotFoundError(f'recomp binary not found: {RECOMP_EXE}')
    if not ORACLE_EXE.exists():
        raise FileNotFoundError(f'oracle binary not found: {ORACLE_EXE}')
    _kill_existing()
    time.sleep(0.5)
    subprocess.Popen(
        [str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    subprocess.Popen(
        [str(ORACLE_EXE), '--paused'],
        cwd=str(ORACLE_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    # Wait for both ports to come up.
    deadline = time.time() + 15
    while time.time() < deadline:
        if _ports_ready():
            time.sleep(0.3)  # small grace so debug server is fully listening
            return
        time.sleep(0.2)
    raise RuntimeError('timeout waiting for smw.exe TCP ports')


def shutdown_pair():
    _kill_existing()


def _ports_ready() -> bool:
    for port in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=0.3)
            s.close()
        except (OSError, ConnectionRefusedError):
            return False
    return True


# ---- TCP client -----------------------------------------------------------

class DebugClient:
    def __init__(self, port: int):
        self.port = port
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
        self.f = self.sock.makefile('rwb')
        self.f.readline()  # consume banner

    def cmd(self, line: str) -> dict:
        self.sock.sendall((line + '\n').encode())
        return json.loads(self.f.readline())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---- snapshot comparison --------------------------------------------------

def snapshot(client: DebugClient) -> Dict[str, bytes]:
    """Return a dict of region -> bytes for diffing. WRAM via read_ram
    (full 128KB). VRAM/CGRAM/OAM via dedicated dump commands if available.
    """
    out: Dict[str, bytes] = {}
    # WRAM: read_ram chunks (command caps at 1024 bytes per call).
    wram = bytearray(0x20000)
    chunk = 1024
    for off in range(0, 0x20000, chunk):
        r = client.cmd(f'read_ram 0x{off:x} {chunk}')
        hexs = r.get('hex', '')
        raw = bytes(int(x, 16) for x in hexs.split()) if hexs else b''
        if len(raw) != chunk:
            raise RuntimeError(f'read_ram at {off:#x} returned {len(raw)} bytes')
        wram[off:off + chunk] = raw
    out['wram'] = bytes(wram)
    # VRAM: dump_vram returns hex over TCP. 64KB VRAM == ~128KB hex; use
    # the client's existing makefile.readline() which handles arbitrary-
    # length lines (and keeps in sync with the makefile's internal buffer,
    # which a raw sock.recv would bypass).
    r = client.cmd('dump_vram 0 65536')
    hex_str = r.get('hex', '')
    vram = bytes.fromhex(hex_str) if hex_str else b''
    if len(vram) != 65536:
        raise RuntimeError(
            f'dump_vram returned {len(vram)} bytes, expected 65536 — response keys: {list(r.keys())}'
        )
    out['vram'] = vram
    if os.environ.get('L3_DEBUG'):
        print(f'[L3] port {client.port} VRAM[0:16] = {vram[:16].hex(" ")}', file=sys.stderr)
        print(f'[L3] port {client.port} WRAM[0:16] = {out["wram"][:16].hex(" ")}', file=sys.stderr)
        print(f'[L3] port {client.port} WRAM[$D7C:$D82] = {out["wram"][0xD7C:0xD82].hex(" ")}', file=sys.stderr)
    return out


def diff_snapshots(a: Dict[str, bytes], b: Dict[str, bytes]) -> Dict[str, List[Tuple[int, int, int]]]:
    """For each region, return list of (offset, a_byte, b_byte) where they
    differ. Capped at 4096 entries per region so failure output stays small."""
    out: Dict[str, List[Tuple[int, int, int]]] = {}
    for k in sorted(set(a.keys()) | set(b.keys())):
        av = a.get(k, b'')
        bv = b.get(k, b'')
        diffs: List[Tuple[int, int, int]] = []
        n = min(len(av), len(bv))
        for i in range(n):
            if av[i] != bv[i]:
                diffs.append((i, av[i], bv[i]))
                if len(diffs) >= 4096:
                    break
        if len(av) != len(bv):
            diffs.append((n, len(av), len(bv)))  # length mismatch marker
        if diffs:
            out[k] = diffs
    return out


# ---- the main entry point -------------------------------------------------

def run_load_only(fixture: str) -> Dict[str, List[Tuple[int, int, int]]]:
    """Sanity test: load the same fixture on both sides and diff state
    without invoking anything. Should always be empty diff if load_state
    is correct. Returns the diff (empty dict = match)."""
    launch_pair()
    try:
        fixture_path = FIXTURES_DIR / fixture
        if not fixture_path.exists():
            raise FileNotFoundError(f'fixture not found: {fixture_path}')
        fixture_abs = str(fixture_path).replace('\\', '/')

        r_client = DebugClient(RECOMP_PORT)
        o_client = DebugClient(ORACLE_PORT)
        try:
            r = r_client.cmd(f'load_state {fixture_abs}')
            if not r.get('ok'):
                raise RuntimeError(f'recomp load_state failed: {r}')
            r = o_client.cmd(f'load_state {fixture_abs}')
            if not r.get('ok'):
                raise RuntimeError(f'oracle load_state failed: {r}')
            recomp_state = snapshot(r_client)
            oracle_state = snapshot(o_client)
            return diff_snapshots(recomp_state, oracle_state)
        finally:
            r_client.close()
            o_client.close()
    finally:
        shutdown_pair()


def run_func(
    name: str,
    fixture: str,
    emu_pc: Optional[int] = None,
    emu_ret: str = 'rtl',
) -> Dict[str, List[Tuple[int, int, int]]]:
    """Load `fixture` on both sides, invoke `name` on recomp and the
    corresponding emu_pc on oracle, diff post-invoke state.

    - fixture: basename under FIXTURES_DIR (e.g. 'attract_f94.snap').
    - name: recomp function name (must be in the recomp_func_registry).
    - emu_pc: 24-bit ROM address for the interpreter. If None, the harness
      resolves it from the registry entry's rom_addr.
    - emu_ret: 'rts' or 'rtl' — how the function returns.

    Returns the diff dict from diff_snapshots. Empty dict means identical.
    """
    launch_pair()
    try:
        fixture_path = FIXTURES_DIR / fixture
        if not fixture_path.exists():
            raise FileNotFoundError(f'fixture not found: {fixture_path}')
        fixture_abs = str(fixture_path).replace('\\', '/')

        r_client = DebugClient(RECOMP_PORT)
        o_client = DebugClient(ORACLE_PORT)
        try:
            r = r_client.cmd(f'load_state {fixture_abs}')
            if not r.get('ok'):
                raise RuntimeError(f'recomp load_state failed: {r}')
            r = o_client.cmd(f'load_state {fixture_abs}')
            if not r.get('ok'):
                raise RuntimeError(f'oracle load_state failed: {r}')

            r_invoke = r_client.cmd(f'invoke_recomp {name}')
            if not r_invoke.get('ok'):
                raise RuntimeError(f'recomp invoke failed: {r_invoke}')
            if emu_pc is None:
                emu_pc_s = r_invoke.get('rom_addr', '')
                emu_pc = int(emu_pc_s, 16) if emu_pc_s else 0
            o_invoke = o_client.cmd(f'invoke_emu {emu_pc:x} {emu_ret}')
            if not o_invoke.get('ok'):
                raise RuntimeError(f'oracle invoke_emu failed: {o_invoke}')

            recomp_state = snapshot(r_client)
            oracle_state = snapshot(o_client)
            return diff_snapshots(recomp_state, oracle_state)
        finally:
            r_client.close()
            o_client.close()
    finally:
        shutdown_pair()


# ---- known-red marker -----------------------------------------------------

def known_red(reason: str):
    """Decorator: a test that is currently EXPECTED to fail for the given
    reason. If it passes, raises — that's your cue to remove the marker.

    Usage:
        @known_red("BG1 chr upload shortfall — framework bug TBD")
        def test_UploadPlayerGFX():
            diff = run_func(...)
            assert not diff, diff
    """
    def wrap(fn):
        @functools.wraps(fn)
        def inner(*args, **kwargs):
            try:
                fn(*args, **kwargs)
            except AssertionError as e:
                print(f'EXPECTED FAIL ({reason}): {e!s}'[:200])
                return
            raise AssertionError(
                f'unexpectedly passed despite @known_red({reason!r}); '
                f'remove the marker'
            )
        inner._known_red = reason  # type: ignore
        return inner
    return wrap


def format_diff_summary(diff: Dict[str, List[Tuple[int, int, int]]]) -> str:
    if not diff:
        return '(no divergence)'
    parts = []
    for region, entries in diff.items():
        parts.append(f'{region}: {len(entries)} bytes differ')
        for off, rv, ov in entries[:3]:
            parts.append(f'  ${off:05x}: recomp=0x{rv:02x} oracle=0x{ov:02x}')
        if len(entries) > 3:
            parts.append(f'  ... ({len(entries) - 3} more)')
    return '\n'.join(parts)
