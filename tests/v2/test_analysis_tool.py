"""Analysis-only driver stays deterministic and never emits generated C."""

import pathlib
import sys
import tempfile

from _helpers import make_lorom_bank0  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

from v2_analyze import _load_cfgs, build_manifest  # noqa: E402


def test_manifest_from_cfg_roots_is_stable_and_follows_calls():
    rom = make_lorom_bank0({
        0x8000: bytes([0x20, 0x00, 0x90, 0x60]),
        0x9000: bytes([0x60]),
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\nfunc Root 8000 end:8004 entry_mx:1,0\n",
            encoding="utf-8")
        first, first_helpers, first_inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)
        second, second_helpers, second_inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    assert first.to_json() == second.to_json()
    assert len(first.roots) == 1
    assert len(first.nodes) == 2
    assert not first_helpers and not second_helpers
    assert not first_inline and not second_inline


def test_default_roots_are_vectors_not_every_function_boundary():
    image = bytearray(make_lorom_bank0({
        0x8000: bytes([0x20, 0x00, 0x90, 0x60]),
        0x9000: bytes([0x60]),
        0xA000: bytes([0x60]),
    }))
    # Native NMI, native IRQ, emulation RESET.
    image[0x7FEA:0x7FEC] = bytes([0x00, 0x80])
    image[0x7FEE:0x7FF0] = bytes([0x00, 0x80])
    image[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func VectorBody 8000 end:8004\n"
            "func ReachedBoundary 9000 end:9001\n"
            "func UnreachableBoundary a000 end:a001\n",
            encoding="utf-8")
        manifest, _helpers, _inline = build_manifest(
            bytes(image), _load_cfgs(cfg_dir),
            max_insns=128, max_nodes=128)

    assert len(manifest.roots) == 4  # RESET M1X1 overlaps NMI/IRQ M1X1.
    assert {key.pc24 for key in manifest.nodes} == {0x008000, 0x009000}
    assert all(key.pc24 != 0x00A000 for key in manifest.nodes)


def test_reachable_exit_mx_fixed_point_redecodes_caller_continuation():
    # Callee forces 16-bit X.  The caller's LDX immediate is therefore three
    # bytes after return; preserve-by-default would decode only two and walk
    # into the $12 operand as an opcode.
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x00, 0x90,       # JSR $9000
            0xA2, 0x34, 0x12,       # LDX #$1234 (X=0 after callee)
            0x60,
        ]),
        0x9000: bytes([0xC2, 0x10, 0x60]),  # REP #$10; RTS
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func Root 8000 end:8007 entry_mx:1,1\n"
            "func ForceX16 9000 end:9003 entry_mx:1,1\n",
            encoding="utf-8")
        manifest, _helpers, _inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    assert manifest.exit_modes[
        next(key for key in manifest.exit_modes
             if key.pc24 == 0x009000 and (key.m, key.x) == (1, 1))
    ] == (1, 0)
    root = next(node for key, node in manifest.nodes.items()
                if key.pc24 == 0x008000 and (key.m, key.x) == (1, 1))
    assert root.max_pc24 == 0x008006
    assert all("brk_at" not in reason for reason in root.reasons)
