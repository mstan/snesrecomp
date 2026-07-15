"""Stable translation-unit sharding for generated bank sources."""

from __future__ import annotations

import pathlib
import re
from collections.abc import Mapping

from .atomic_output import write_if_changed


_TOP_LEVEL_CPU_FN_RE = re.compile(
    r"^(?:RecompReturn|void)\s+([A-Za-z_]\w*)\s*"
    r"\(CpuState\s*\*cpu\)\s*\{", re.MULTILINE)
_VARIANT_CALL_NAME_RE = re.compile(
    r"\b([A-Za-z_]\w*_M[01]X[01])\s*\(\s*cpu\s*\)")
_VARIANT_SUFFIX_RE = re.compile(r"_M[01]X[01]$")
_SYNTHETIC_BANK_FN_RE = re.compile(
    r"^bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]{4})$")
_FORWARD_DECL_MARKER = "/* Forward declarations for in-bank entries. */"


def split_bank_translation_units(
        source: str, bank: int, symbol_pcs: Mapping[str, int], *,
        threshold_bytes: int, pc_span: int) -> dict[str, str]:
    """Split a large bank source into stable entry-address shards.

    Sharding happens after semantic emission, so analysis still sees the bank
    as one unit. Functions are assigned by ROM PC rather than source size;
    editing one body therefore cannot move unrelated functions between object
    files. Each shard carries only the generated variants its bodies call.
    """
    bank &= 0xFF
    monolithic_name = f"bank{bank:02x}_v2.c"
    if (pc_span <= 0 or threshold_bytes < 0
            or len(source.encode("utf-8")) < threshold_bytes):
        return {monolithic_name: source}

    matches = list(_TOP_LEVEL_CPU_FN_RE.finditer(source))
    if not matches:
        return {monolithic_name: source}

    preamble = source[:matches[0].start()]
    marker = preamble.find(_FORWARD_DECL_MARKER)
    if marker < 0:
        raise ValueError(
            f"bank ${bank:02X}: emitted source lacks forward declarations")
    include_preamble = preamble[:marker].rstrip() + "\n\n"

    chunks: dict[int, list[str]] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() \
            if index + 1 < len(matches) else len(source)
        body = source[match.start():end].strip() + "\n"
        symbol = match.group(1)
        base = _VARIANT_SUFFIX_RE.sub("", symbol)
        pc = symbol_pcs.get(base)
        if pc is None:
            synthetic = _SYNTHETIC_BANK_FN_RE.match(base)
            if synthetic and int(synthetic.group(1), 16) == bank:
                pc = int(synthetic.group(2), 16)
        if pc is None:
            raise ValueError(
                f"bank ${bank:02X}: cannot assign generated function "
                f"{symbol!r} to a stable PC shard")
        pc16 = int(pc) & 0xFFFF
        part = max(0, (pc16 - 0x8000) // pc_span)
        chunks.setdefault(part, []).append(body)

    outputs: dict[str, str] = {}
    for part, bodies in sorted(chunks.items()):
        joined = "\n".join(bodies)
        references = sorted(set(_VARIANT_CALL_NAME_RE.findall(joined)))
        declarations = "".join(
            f"RecompReturn {name}(CpuState *cpu);\n"
            for name in references)
        start = 0x8000 + part * pc_span
        end = min(0xFFFF, start + pc_span - 1)
        part_header = (
            f"/* Split translation unit: bank ${bank:02X}, part {part:02X}; "
            f"entry PCs ${start:04X}-${end:04X}. */\n")
        outputs[f"bank{bank:02x}_part{part:02x}_v2.c"] = (
            include_preamble + part_header + "\n" + declarations + "\n"
            + joined.rstrip() + "\n")
    return outputs


def write_bank_translation_units(
        out_dir: pathlib.Path, bank: int, symbol_pcs: Mapping[str, int],
        source: str, *, threshold_bytes: int,
        pc_span: int) -> tuple[tuple[str, ...], int]:
    """Write one bank's desired shape and remove stale monoliths/shards."""
    outputs = split_bank_translation_units(
        source, bank, symbol_pcs, threshold_bytes=threshold_bytes,
        pc_span=pc_span)
    wanted = set(outputs)
    changed = 0
    for filename, content in outputs.items():
        changed += int(write_if_changed(out_dir / filename, content))
    for old in out_dir.glob(f"bank{bank & 0xFF:02x}*_v2.c"):
        if old.name not in wanted:
            old.unlink()
            changed += 1
    return tuple(sorted(wanted)), changed
