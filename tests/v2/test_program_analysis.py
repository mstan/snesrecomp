"""LLE-first compact whole-program analysis regressions."""

import json

from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function  # noqa: E402
from v2.program_analysis import (  # noqa: E402
    EdgeKind,
    EdgeResolution,
    NodeDisposition,
    ProgramAnalyzer,
    VariantKey,
    summarize_decode_graph,
)


def _decode(rom, *, max_insns=12000, **kwargs):
    def run(key):
        return decode_function(
            rom, (key.pc24 >> 16) & 0xFF, key.pc24 & 0xFFFF,
            key.m, key.x, max_insns=max_insns, **kwargs)
    return run


def test_direct_call_closure_preserves_exact_mx_and_is_deterministic():
    rom = make_lorom_bank0({
        0x8000: bytes([0x20, 0x00, 0x90, 0x60]),  # JSR $9000; RTS
        0x9000: bytes([0x60]),
    })
    analyzer = ProgramAnalyzer(_decode(rom))
    manifest = analyzer.analyze([VariantKey(0x008000, 1, 0)])

    assert sorted(manifest.nodes) == [
        VariantKey(0x008000, 1, 0), VariantKey(0x009000, 1, 0)]
    edge, = manifest.nodes[VariantKey(0x008000, 1, 0)].demands
    assert edge.kind == EdgeKind.DIRECT_CALL
    assert edge.resolution == EdgeResolution.AOT_EXACT
    assert edge.target == VariantKey(0x009000, 1, 0)
    assert manifest.to_json() == analyzer.analyze(
        [VariantKey(0x008000, 1, 0)]).to_json()
    assert list(json.loads(manifest.to_json())["nodes"]) == [
        "008000:M1X0", "009000:M1X0"]


def test_unresolved_indirect_is_per_edge_lle_and_keeps_direct_demand():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x00, 0x90,  # JSR $9000
            0x6C, 0x10, 0x00,  # JMP ($0010), unresolved runtime pointer
        ]),
        0x9000: bytes([0x60]),
    })
    manifest = ProgramAnalyzer(_decode(rom)).analyze([
        VariantKey(0x008000, 1, 1)])
    root = manifest.nodes[VariantKey(0x008000, 1, 1)]

    assert root.disposition == NodeDisposition.AOT_ELIGIBLE
    assert "has_lle_indirect_edge" in root.reasons
    assert {edge.kind for edge in root.demands} == {
        EdgeKind.DIRECT_CALL, EdgeKind.UNRESOLVED_INDIRECT}
    dynamic = next(edge for edge in root.demands
                   if edge.kind == EdgeKind.UNRESOLVED_INDIRECT)
    assert dynamic.resolution == EdgeResolution.LLE_DYNAMIC
    assert dynamic.target is None
    assert VariantKey(0x009000, 1, 1) in manifest.nodes


def test_structural_poison_does_not_propagate_plausible_calls():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x00, 0x90,  # plausible JSR in a poisoned stream
            0x00, 0x00,        # BRK
        ]),
        0x9000: bytes([0x60]),
    })
    manifest = ProgramAnalyzer(_decode(rom)).analyze([
        VariantKey(0x008000, 1, 1)])
    root = manifest.nodes[VariantKey(0x008000, 1, 1)]

    assert root.disposition == NodeDisposition.LLE_ONLY
    assert "structural_poison" in root.reasons
    assert not root.demands
    assert VariantKey(0x009000, 1, 1) not in manifest.nodes


def test_decode_budget_classifies_node_lle_only_without_partial_graph():
    rom = make_lorom_bank0({
        0x8000: bytes([0xEA, 0xEA, 0xEA, 0x60]),
    })
    manifest = ProgramAnalyzer(_decode(rom, max_insns=2)).analyze([
        VariantKey(0x008000, 1, 1)])
    node = manifest.nodes[VariantKey(0x008000, 1, 1)]

    assert node.disposition == NodeDisposition.LLE_ONLY
    assert node.reasons == ("decode_budget_exhausted",)
    assert node.instruction_count == 0
    assert not node.demands


def test_invalid_static_target_is_recorded_as_exact_lle_not_enqueued():
    rom = make_lorom_bank0({
        0x8000: bytes([0x22, 0x00, 0x80, 0x01, 0x60]),  # JSL $018000
    })
    root_key = VariantKey(0x008000, 1, 1)
    analyzer = ProgramAnalyzer(
        _decode(rom), target_is_code=lambda target: target.pc24 < 0x010000)
    manifest = analyzer.analyze([root_key])
    edge, = manifest.nodes[root_key].demands

    assert edge.target == VariantKey(0x018000, 1, 1)
    assert edge.resolution == EdgeResolution.LLE_EXACT
    assert edge.target not in manifest.nodes


def test_cross_bank_long_jump_is_an_exact_tail_demand():
    rom = make_lorom_bank0({
        0x8000: bytes([0x5C, 0x00, 0x90, 0x00]),  # JML $009000
        0x9000: bytes([0x60]),
    })
    manifest = ProgramAnalyzer(_decode(rom)).analyze([
        VariantKey(0x008000, 0, 1)])
    edge, = manifest.nodes[VariantKey(0x008000, 0, 1)].demands

    assert edge.kind == EdgeKind.DIRECT_TAIL_CALL
    assert edge.target == VariantKey(0x009000, 0, 1)
    assert edge.target in manifest.nodes
