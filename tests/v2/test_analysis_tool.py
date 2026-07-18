"""Analysis-only driver stays deterministic and never emits generated C."""

import pathlib
import sys
import tempfile

from _helpers import make_lorom_bank0  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

from v2_analyze import (  # noqa: E402
    _load_cfgs,
    _solve_exit_equation_sccs,
    build_manifest,
    build_manifest_native,
    native_analyzer_path,
    native_unsupported_features,
)
from v2.program_analysis import NodeDisposition, VariantKey  # noqa: E402
from v2.program_emit import build_emission_entries  # noqa: E402


def test_exit_equation_solver_bootstraps_closed_recursive_component():
    first = VariantKey(0xB98000, 0, 0)
    second = VariantKey(0xB98100, 0, 0)
    blocked = VariantKey(0xB98200, 0, 0)
    equations = {
        first: ({(0, 0)}, {(second.pc24, 0, 0)}),
        second: (set(), {(first.pc24, 0, 0)}),
        blocked: (set(), {(0xB9F000, 0, 0)}),
    }
    solved = _solve_exit_equation_sccs(equations, {}, {})
    assert solved[first] == frozenset({(0, 0)})
    assert solved[second] == frozenset({(0, 0)})
    assert blocked not in solved


def test_exit_equation_solver_rejects_false_preservation_probe():
    caller = VariantKey(0xB98000, 0, 0)
    callee = VariantKey(0xB98100, 0, 0)
    callee_dep = (callee.pc24, 0, 0)
    equations = {
        caller: ({(0, 0)}, {callee_dep}, {(callee_dep, 0, 0)}),
        callee: ({(1, 0)}, {(caller.pc24, 0, 0)}, frozenset()),
    }
    assert _solve_exit_equation_sccs(equations, {}, {}) == {}


def test_exit_equation_solver_preserves_closed_noreturn_fact():
    loop = VariantKey(0xB98000, 0, 0)
    solved = _solve_exit_equation_sccs(
        {loop: (set(), set())}, {}, {})
    assert solved[loop] == frozenset()


def test_probe_requirement_does_not_become_caller_exit():
    caller = VariantKey(0xB98000, 0, 0)
    helper = VariantKey(0xB98100, 0, 0)
    helper_dep = (helper.pc24, 0, 0)
    solved = _solve_exit_equation_sccs({
        caller: (set(), set(), {(helper_dep, 0, 0)}),
        helper: ({(0, 0)}, set(), frozenset()),
    }, {}, {})
    assert solved[caller] == frozenset()
    assert solved[helper] == frozenset({(0, 0)})


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


def test_native_manifest_matches_python_ptrcall_emission_contract():
    """CI exercises the native boundary; source-only users may skip it."""
    if not native_analyzer_path().is_file():
        return
    rom = make_lorom_bank0({
        0x8000: bytes([0xF4, 0x08, 0x80, 0x6C, 0x10, 0x00]),
        0x8009: bytes([0x60]),
        0x8010: bytes([0x60]),
    })
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        cfg_dir = root / "cfg"
        cfg_dir.mkdir()
        rom_path = root / "fixture.sfc"
        rom_path.write_bytes(rom)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "indirect_dispatch 8003 1 ptrcall targets:8010\n"
            "func Root 8000 end:800a entry_mx:1,1\n"
            "func Handler 8010 end:8011 entry_mx:1,1\n",
            encoding="utf-8")
        expected, _helpers, _inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=4096, max_nodes=100_000,
            all_cfg_roots=True)
        actual, _helpers, _inline, _output = build_manifest_native(
            rom_path=rom_path, cfg_dir=cfg_dir, all_cfg_roots=True)

    assert actual.roots == expected.roots
    assert set(actual.nodes) == set(expected.nodes)
    assert actual.exit_modes == expected.exit_modes
    assert actual.exit_mode_sets == expected.exit_mode_sets
    assert {
        key: node.disposition for key, node in actual.nodes.items()
    } == {
        key: node.disposition for key, node in expected.nodes.items()
    }


def test_ptrcall_is_guarded_until_full_project_contract_matches():
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "indirect_dispatch 8003 1 ptrcall targets:8010\n"
            "func Root 8000\n",
            encoding="utf-8")
        unsupported = native_unsupported_features(_load_cfgs(cfg_dir))
    assert unsupported == ("indirect_dispatch ptrcall",)


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
    assert root.disposition == NodeDisposition.AOT_ELIGIBLE


def test_proven_noreturn_callee_does_not_poison_caller_continuation():
    rom = make_lorom_bank0({
        0x8000: bytes([0x20, 0x00, 0x90, 0x00]),  # JSR $9000; dead BRK
        0x9000: bytes([0xCB]),                    # WAI: no return path
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func Root 8000 end:8004 entry_mx:1,1\n"
            "func WaitForever 9000 end:9001 entry_mx:1,1\n",
            encoding="utf-8")
        manifest, _helpers, _inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    root_key = VariantKey(0x008000, 1, 1)
    wait_key = VariantKey(0x009000, 1, 1)
    assert manifest.exit_mode_sets[wait_key] == frozenset()
    assert manifest.exit_mode_sets[root_key] == frozenset()
    assert manifest.nodes[root_key].disposition == NodeDisposition.AOT_ELIGIBLE
    assert manifest.nodes[root_key].max_pc24 == 0x008000


def test_recursive_unknown_exit_component_converges_to_lle():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x00, 0x90,       # JSR $9000
            0x60,                   # RTS
        ]),
        0x9000: bytes([
            0x20, 0x00, 0x80,       # JSR $8000
            0xC2, 0x20,             # REP #$20
            0x60,                   # RTS
        ]),
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func RecursiveA 8000 end:8004 entry_mx:1,1\n"
            "func RecursiveB 9000 end:9006 entry_mx:1,1\n",
            encoding="utf-8")
        manifest, _helpers, _inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    assert not manifest.exit_modes
    assert len(manifest.nodes) == 2
    assert all(node.disposition == NodeDisposition.LLE_ONLY
               for node in manifest.nodes.values())
    assert all("unproven_callee_exit" in node.reasons
               for node in manifest.nodes.values())


def test_lorom_mirror_uses_declared_function_boundaries():
    rom = make_lorom_bank0({
        0x8000: bytes([0x5C, 0x00, 0x81, 0x80]),  # JML $80:8100
        0x8100: bytes([0x4C, 0x00, 0x82]),        # JMP $8200
        0x8200: bytes([0x60]),                    # RTS
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func Root 8000 end:8004 entry_mx:1,1\n"
            "func MirroredTarget 8100 end:8103 entry_mx:1,1\n"
            "func MirroredSibling 8200 end:8201 entry_mx:1,1\n",
            encoding="utf-8")
        manifest, _helpers, _inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    mirrored = manifest.nodes[next(
        key for key in manifest.nodes
        if key.pc24 == 0x808100 and (key.m, key.x) == (1, 1))]
    assert mirrored.instruction_count == 1
    assert any(edge.target is not None
               and edge.target.pc24 == 0x808200
               for edge in mirrored.demands)


def test_hle_overlay_preserves_entry_mx_for_caller_analysis():
    rom = make_lorom_bank0({
        0x8000: bytes([0x20, 0x00, 0x90, 0x60]),  # JSR $9000; RTS
        # The ROM body is a non-returning scheduler transfer. The HLE overlay
        # is the callable boundary and therefore uses the preserve-M/X ABI.
        0x9000: bytes([0x80, 0xFE]),              # BRA $9000
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func Root 8000 end:8004 entry_mx:1,1\n"
            "func YieldOverlay 9000 end:9002 entry_mx:1,1\n"
            "hle_func 9000 HleYieldOverlay\n",
            encoding="utf-8")
        manifest, _helpers, _inline = build_manifest(
            rom, _load_cfgs(cfg_dir), max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    root_key = next(key for key in manifest.nodes
                    if key.pc24 == 0x008000 and (key.m, key.x) == (1, 1))
    assert manifest.nodes[root_key].disposition == NodeDisposition.AOT_ELIGIBLE
    assert manifest.exit_modes[root_key] == (1, 1)


def test_hle_overlay_contract_applies_to_lorom_execution_mirror():
    rom = make_lorom_bank0({
        0x8000: bytes([0x5C, 0x00, 0x81, 0x80]),  # JML $80:8100
        0x8100: bytes([0x20, 0x00, 0x90, 0x60]),  # JSR $9000; RTS
        0x9000: bytes([0x80, 0xFE]),              # BRA $9000
    })
    with tempfile.TemporaryDirectory() as directory:
        cfg_dir = pathlib.Path(directory)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 00\n"
            "func Root 8000 end:8004 entry_mx:1,1\n"
            "func MirroredCaller 8100 end:8104 entry_mx:1,1\n"
            "func YieldOverlay 9000 end:9002 entry_mx:1,1\n"
            "hle_func 9000 HleYieldOverlay\n",
            encoding="utf-8")
        parsed = _load_cfgs(cfg_dir)
        manifest, _helpers, _inline = build_manifest(
            rom, parsed, max_insns=128, max_nodes=128,
            all_cfg_roots=True)

    caller_key = next(key for key in manifest.nodes
                      if key.pc24 == 0x808100
                      and (key.m, key.x) == (1, 1))
    assert manifest.nodes[caller_key].disposition == NodeDisposition.AOT_ELIGIBLE
    assert manifest.exit_modes[caller_key] == (1, 1)
    entries_by_bank, emitted, _names, _cfgs = build_emission_entries(
        manifest, parsed)
    assert emitted[0x809000] == {
        (0, 0), (0, 1), (1, 0), (1, 1)
    }
    assert {
        (entry.entry_m, entry.entry_x)
        for entry in entries_by_bank[0x80]
        if entry.start == 0x9000
    } == {(0, 0), (0, 1), (1, 0), (1, 1)}
