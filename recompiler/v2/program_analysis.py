"""Compact, deterministic whole-program variant analysis.

This module deliberately does not emit C.  It reduces each transient
``FunctionDecodeGraph`` to a small immutable summary, then follows exact
``(pc24, M, X)`` demands to a fixed point.  Dynamic or unresolved transfers
remain explicit LLE edges instead of inventing a generated target variant.

The first integration stage is intentionally side-by-side with the legacy
emission-feedback loop.  A later stage can compare these manifests against
the generated program before making this graph authoritative for emission.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from enum import Enum
import hashlib
import heapq
import json
from typing import Callable, Iterable, Mapping, Optional, Tuple

from snes65816 import INDIR_X

from .decoder import FunctionDecodeGraph


class NodeDisposition(str, Enum):
    """Materialization state from the LLE-first analysis contract."""

    LLE_ONLY = "lle_only"
    AOT_ELIGIBLE = "aot_eligible"
    AOT_EMITTED = "aot_emitted"
    HLE_OVERLAY = "hle_overlay"


class EdgeKind(str, Enum):
    DIRECT_CALL = "direct_call"
    STATIC_DISPATCH = "static_dispatch"
    DYNAMIC_DISPATCH = "dynamic_dispatch"
    UNRESOLVED_INDIRECT = "unresolved_indirect"
    SUPPRESSED_INDIRECT_CALL = "suppressed_indirect_call"


class EdgeResolution(str, Enum):
    AOT_EXACT = "aot_exact"
    LLE_EXACT = "lle_exact"
    LLE_DYNAMIC = "lle_dynamic"


@dataclass(frozen=True, order=True)
class VariantKey:
    pc24: int
    m: int
    x: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "pc24", self.pc24 & 0xFFFFFF)
        object.__setattr__(self, "m", self.m & 1)
        object.__setattr__(self, "x", self.x & 1)

    @property
    def manifest_key(self) -> str:
        return f"{self.pc24:06X}:M{self.m}X{self.x}"


@dataclass(frozen=True, order=True)
class DemandEdge:
    site_pc24: int
    kind: EdgeKind
    resolution: EdgeResolution
    target: Optional[VariantKey] = None
    detail: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "site_pc24", self.site_pc24 & 0xFFFFFF)


@dataclass(frozen=True)
class NodeSummary:
    key: VariantKey
    disposition: NodeDisposition
    instruction_count: int
    min_pc24: int
    max_pc24: int
    demands: Tuple[DemandEdge, ...] = ()
    reasons: Tuple[str, ...] = ()
    digest: str = ""

    @property
    def propagates_demands(self) -> bool:
        # Structural poison is a wrong-width/invalid decode and must not
        # manufacture transitive reachability.  Ordinary LLE-only nodes can
        # still carry proven direct demands.
        return "structural_poison" not in self.reasons


@dataclass(frozen=True)
class ProgramManifest:
    roots: Tuple[VariantKey, ...]
    nodes: Mapping[VariantKey, NodeSummary]
    format_version: int = 1

    def to_dict(self) -> dict:
        def key_dict(key: VariantKey) -> dict:
            return {"pc24": key.pc24, "m": key.m, "x": key.x}

        def edge_dict(edge: DemandEdge) -> dict:
            return {
                "site_pc24": edge.site_pc24,
                "kind": edge.kind.value,
                "resolution": edge.resolution.value,
                "target": (key_dict(edge.target) if edge.target else None),
                "detail": edge.detail,
            }

        def node_dict(node: NodeSummary) -> dict:
            return {
                "key": key_dict(node.key),
                "disposition": node.disposition.value,
                "instruction_count": node.instruction_count,
                "min_pc24": node.min_pc24,
                "max_pc24": node.max_pc24,
                "demands": [edge_dict(e) for e in node.demands],
                "reasons": list(node.reasons),
                "digest": node.digest,
            }

        return {
            "format_version": self.format_version,
            "roots": [key_dict(k) for k in sorted(self.roots)],
            "nodes": {
                key.manifest_key: node_dict(self.nodes[key])
                for key in sorted(self.nodes)
            },
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n"


def _target_key(site_pc24: int, raw_target: int, kind: str,
                m: int, x: int) -> VariantKey:
    if kind == "long" or raw_target > 0xFFFF:
        pc24 = raw_target & 0xFFFFFF
    else:
        pc24 = (site_pc24 & 0xFF0000) | (raw_target & 0xFFFF)
    return VariantKey(pc24, m, x)


def _stable_summary_digest(key: VariantKey, disposition: NodeDisposition,
                           instruction_rows: list, demands: Tuple[DemandEdge, ...],
                           reasons: Tuple[str, ...]) -> str:
    payload = {
        "key": asdict(key),
        "disposition": disposition.value,
        "instructions": instruction_rows,
        "demands": [
            {
                "site": edge.site_pc24,
                "kind": edge.kind.value,
                "resolution": edge.resolution.value,
                "target": asdict(edge.target) if edge.target else None,
                "detail": edge.detail,
            }
            for edge in demands
        ],
        "reasons": reasons,
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def summarize_decode_graph(
    graph: FunctionDecodeGraph,
    *,
    target_is_code: Optional[Callable[[VariantKey], bool]] = None,
) -> NodeSummary:
    """Reduce a transient decode graph to deterministic reachability facts.

    Unresolved indirect sites are represented as LLE edges while direct calls
    discovered elsewhere in the same graph remain valid AOT demands.  A graph
    containing BRK/COP is treated as structural poison: it stays reachable via
    LLE, but none of its speculative outgoing demands are propagated.
    """
    key = VariantKey(graph.entry.pc, graph.entry.m, graph.entry.x)
    rows = []
    edges = set()
    poison_reasons = set()

    for decode_key, decoded in sorted(
            graph.insns.items(), key=lambda item: (
                item[0].pc, item[0].m, item[0].x, item[0].p_stack)):
        insn = decoded.insn
        site = insn.addr & 0xFFFFFF
        rows.append((site, decode_key.m, decode_key.x, insn.opcode,
                     insn.length, tuple(
                         (s.pc, s.m, s.x) for s in decoded.successors)))

        if insn.mnem in ("BRK", "COP"):
            poison_reasons.add(f"{insn.mnem.lower()}_at_{site:06X}")

        entries = getattr(insn, "dispatch_entries", None)
        if entries and not getattr(insn, "dispatch_local_goto", False):
            dispatch_kind = getattr(insn, "dispatch_kind", None) or "short"
            for raw_target in entries:
                if not raw_target:
                    continue
                target = _target_key(site, int(raw_target), dispatch_kind,
                                     insn.m_flag, insn.x_flag)
                resolution = (EdgeResolution.AOT_EXACT
                              if target_is_code is None or target_is_code(target)
                              else EdgeResolution.LLE_EXACT)
                edges.add(DemandEdge(
                    site, EdgeKind.STATIC_DISPATCH, resolution, target))

        # Dispatch-helper JSL/JML instructions carry both a helper operand
        # and the actual handler entries.  The entries are the program demand;
        # the helper is an implementation detail and must not become a node.
        if entries:
            continue

        if insn.mnem == "JSL":
            target = VariantKey(insn.operand, insn.m_flag, insn.x_flag)
            resolution = (EdgeResolution.AOT_EXACT
                          if target_is_code is None or target_is_code(target)
                          else EdgeResolution.LLE_EXACT)
            edges.add(DemandEdge(
                site, EdgeKind.DIRECT_CALL, resolution, target))
        elif insn.mnem == "JSR" and insn.mode != INDIR_X:
            target = VariantKey(
                (site & 0xFF0000) | (insn.operand & 0xFFFF),
                insn.m_flag, insn.x_flag)
            resolution = (EdgeResolution.AOT_EXACT
                          if target_is_code is None or target_is_code(target)
                          else EdgeResolution.LLE_EXACT)
            edges.add(DemandEdge(
                site, EdgeKind.DIRECT_CALL, resolution, target))

        if getattr(insn, "dispatch_runtime", False):
            edges.add(DemandEdge(
                site, EdgeKind.DYNAMIC_DISPATCH,
                EdgeResolution.LLE_DYNAMIC,
                detail=f"table_base={insn.operand & 0xFFFF:04X}"))

    for item in graph.unresolved_indirects:
        edges.add(DemandEdge(
            item.site_pc24, EdgeKind.UNRESOLVED_INDIRECT,
            EdgeResolution.LLE_DYNAMIC,
            detail=f"{item.mnem}:mode={item.mode}:operand={item.operand:06X}"))
    for item in graph.suppressed_indirect_calls:
        edges.add(DemandEdge(
            item.site_pc24, EdgeKind.SUPPRESSED_INDIRECT_CALL,
            EdgeResolution.LLE_DYNAMIC,
            detail=f"table_base={item.table_base:04X}"))

    reasons = set()
    if not graph.insns:
        reasons.update(("structural_poison", "empty_decode"))
    if poison_reasons:
        reasons.add("structural_poison")
        reasons.update(poison_reasons)
    if graph.unresolved_indirects:
        reasons.add("has_lle_indirect_edge")
    if graph.suppressed_indirect_calls:
        reasons.add("has_lle_suppressed_call_edge")

    disposition = (NodeDisposition.LLE_ONLY if "structural_poison" in reasons
                   else NodeDisposition.AOT_ELIGIBLE)
    demands = tuple(sorted(edges))
    if "structural_poison" in reasons:
        # Wrong-width garbage often contains plausible JSR/JSL bytes.  Keep
        # them in the digest through `rows`, but never grow the program graph.
        demands = ()
    reason_tuple = tuple(sorted(reasons))
    pcs = [row[0] for row in rows]
    digest = _stable_summary_digest(
        key, disposition, rows, demands, reason_tuple)
    return NodeSummary(
        key=key,
        disposition=disposition,
        instruction_count=len(rows),
        min_pc24=min(pcs, default=key.pc24),
        max_pc24=max(pcs, default=key.pc24),
        demands=demands,
        reasons=reason_tuple,
        digest=digest,
    )


class ProgramAnalyzer:
    """Deterministic fixed-point traversal over exact variant demand."""

    def __init__(self, decode_variant: Callable[[VariantKey], FunctionDecodeGraph],
                 *, max_nodes: int = 100_000,
                 target_is_code: Optional[Callable[[VariantKey], bool]] = None):
        self._decode_variant = decode_variant
        self._max_nodes = max_nodes
        self._target_is_code = target_is_code

    def analyze(self, roots: Iterable[VariantKey]) -> ProgramManifest:
        roots = tuple(sorted(set(roots)))
        pending = list(roots)
        heapq.heapify(pending)
        queued = set(roots)
        nodes = {}

        while pending:
            key = heapq.heappop(pending)
            if key in nodes:
                continue
            if len(nodes) >= self._max_nodes:
                raise RuntimeError(
                    f"program analysis exceeded max_nodes={self._max_nodes}")

            try:
                graph = self._decode_variant(key)
            except RuntimeError as exc:
                if "exceeded max_insns=" not in str(exc):
                    raise
                reason_tuple = ("decode_budget_exhausted",)
                digest = _stable_summary_digest(
                    key, NodeDisposition.LLE_ONLY, [], (), reason_tuple)
                nodes[key] = NodeSummary(
                    key, NodeDisposition.LLE_ONLY, 0, key.pc24, key.pc24,
                    reasons=reason_tuple, digest=digest)
                continue

            summary = summarize_decode_graph(
                graph, target_is_code=self._target_is_code)
            if summary.key != key:
                raise ValueError(
                    f"decoder returned {summary.key} for requested {key}")
            nodes[key] = summary
            if not summary.propagates_demands:
                continue
            for edge in summary.demands:
                if (edge.resolution == EdgeResolution.AOT_EXACT
                        and edge.target is not None
                        and edge.target not in nodes
                        and edge.target not in queued):
                    heapq.heappush(pending, edge.target)
                    queued.add(edge.target)

        return ProgramManifest(roots=roots, nodes=nodes)
