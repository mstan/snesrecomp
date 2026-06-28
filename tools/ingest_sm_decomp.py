"""Harvest function-definition PC comments from the snesrev/sm decomp and
emit them as `func <Name> <hex_pc> end:<hex_next>` cfg lines per bank.

This is the Super Metroid analogue of `ingest_zelda3_decomp.py`. The
snesrev/sm decomp (https://github.com/snesrev/sm) is the same lineage as
snesrev/zelda3 and snesrev/smw: every function definition carries a
trailing comment with its original SNES PC. The ONLY format difference
from zelda3 is the PC literal — sm writes it with a `0x` prefix and
mixed-case, variable-width hex:

    void APU_UploadBank(uint32 addr) {  // 0x808028
    CoroutineRet WaitForNMI_Async(void) {  // 0x808338
    uint8 *RomPtr(uint32_t addr) {  // (helper, no PC — skipped)

zelda3, by contrast, wrote bare 6-hex: `// 8080c9`. Everything else
(per-bank emit, `end:` boundary computation, hand-declared-func
suppression, LoROM $80-$FF -> $00-$7F mirror normalisation, idempotent
auto-section replacement) is identical to the zelda3 ingester; see that
file's module docstring for the rationale on `func`+`end:` vs `name`
aliases and the tail-call fall-through semantics.

Non-function trailing-PC lines (e.g. `static Func_V *const
kIrqHandlers[14] = {  // 0x80986A`) are NOT matched: the regex requires
a `(...)` parameter list, which array/table declarations lack.

Super Metroid is a 3 MB (24 Mbit) LoROM: code banks $80-$DF mirror
physical banks $00-$5F. The `& 0x7F` normalisation handles this exactly
as it does Zelda's 1 MB ($80-$9F) layout.

Idempotent: each cfg file's auto-ingested section is delimited and
replaced wholesale on every run; hand-written `func` lines outside the
markers always win.

Usage:
    python tools/ingest_sm_decomp.py
        [--decomp F:/Projects/sm]
        [--output F:/Projects/snesrecomp/SuperMetroidRecomp/recomp]
        [--dry-run]
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import List, Tuple

# Match the function-definition-opening-brace line shape:
#   [qualifiers] <return-type>[*] <Name>(<args>) {  // 0x<pc>
# - qualifiers: optional static/extern/const/inline, repeated
# - return type: one identifier word (uint8, void, CoroutineRet, ...)
# - args: any single-line text (the decomp keeps signatures on one line)
# - pc: `0x` + 4..6 hex digits, mixed case (sm style); trailing nothing
DEFN_RE = re.compile(
    r'^\s*'
    r'(?:(?:static|extern|const|inline)\s+)*'      # optional qualifiers
    r'[A-Za-z_]\w*'                                # return type word
    r'[\s\*]+'                                     # whitespace and/or stars
    r'([A-Za-z_]\w*)'                              # function name (group 1)
    r'\s*\([^)]*\)\s*\{'                           # ( args ) {
    r'\s*//\s*'                                    # // comment leader
    r'0x([0-9a-fA-F]{4,6})'                        # 0x<pc> (group 2)
    r'\s*$'                                        # nothing else after
)

INGEST_BEGIN = (
    "# >>> AUTO-INGESTED FROM sm DECOMP "
    "— do not hand-edit between markers >>>"
)
INGEST_END = "# <<< END AUTO-INGESTED <<<"

# Skip dirs that contain no SNES function definitions.
_SKIP_DIR_PARTS = {"assets", "other", ".git", ".github", "build", "saves",
                   "third_party", "platform", "snes"}


def harvest(decomp_root: Path) -> List[Tuple[int, int, str]]:
    """Walk decomp .c files, return list of (bank, pc16, name)."""
    entries: List[Tuple[int, int, str]] = []
    seen_pc24 = set()
    for path in sorted(decomp_root.rglob("*.c")):
        if any(part in _SKIP_DIR_PARTS for part in path.parts):
            continue
        with open(path, encoding="utf-8", errors="replace") as fp:
            for line in fp:
                m = DEFN_RE.match(line)
                if not m:
                    continue
                name = m.group(1)
                pc24 = int(m.group(2), 16)
                if pc24 in seen_pc24:
                    continue
                seen_pc24.add(pc24)
                # LoROM mirror: $80-$FF banks map to $00-$7F physical banks.
                bank = (pc24 >> 16) & 0x7F
                addr = pc24 & 0xFFFF
                # ROM bytes only live in $8000-$FFFF for LoROM-bank entries.
                if not (0x8000 <= addr <= 0xFFFF):
                    continue
                entries.append((bank, addr, name))
    return entries


def emit_per_bank(
    entries: List[Tuple[int, int, str]],
    output_dir: Path,
    dry_run: bool = False,
) -> None:
    by_bank: dict[int, List[Tuple[int, str]]] = defaultdict(list)
    for bank, addr, name in entries:
        by_bank[bank].append((addr, name))

    ingest_section_re = re.compile(
        re.escape(INGEST_BEGIN) + r".*?" + re.escape(INGEST_END) + r"\n?",
        flags=re.DOTALL,
    )
    # Hand-written `func <name> <pc>` lines outside the auto-section always
    # win — auto-ingested entries at the same PC are skipped to avoid
    # duplicate C function definitions.
    func_decl_re = re.compile(r'^\s*func\s+(\S+)\s+([0-9a-fA-F]+)\b')

    for bank in sorted(by_bank):
        items = sorted(by_bank[bank])
        # Dedupe by addr (decomp per-file order keeps first encountered).
        seen = set()
        dedup: List[Tuple[int, str]] = []
        for addr, name in items:
            if addr in seen:
                continue
            seen.add(addr)
            dedup.append((addr, name))

        cfg_path = output_dir / f"bank{bank:02x}.cfg"
        # Pre-read hand-declared func PCs (everything OUTSIDE the auto
        # section). Auto-emit skips these PCs so the hand-written entry's
        # attributes (end:, exit_mx, etc.) are the only declaration the
        # loader sees.
        hand_pcs: set = set()
        hand_block = ""
        if cfg_path.exists():
            existing = cfg_path.read_text(encoding="utf-8")
            hand_block = ingest_section_re.sub("", existing)
            for ln in hand_block.splitlines():
                m = func_decl_re.match(ln)
                if not m:
                    continue
                try:
                    hand_pcs.add(int(m.group(2), 16) & 0xFFFF)
                except ValueError:
                    pass

        filtered = [(addr, name) for (addr, name) in dedup
                    if addr not in hand_pcs]

        section_lines = [
            INGEST_BEGIN,
            "# Source: snesrev/sm decomp PC comments.",
            "# Regenerate via: python tools/ingest_sm_decomp.py",
            f"# {len(filtered)} entries "
            f"({len(dedup) - len(filtered)} suppressed by hand-declared func).",
        ]
        # `end:` = next entry's PC; last entry caps at 0x10000. Fall-through
        # past `end:` into the next entry routes through the tail-call
        # codegen in emit_function.py.
        for i, (addr, name) in enumerate(filtered):
            next_addr = filtered[i + 1][0] if i + 1 < len(filtered) else 0x10000
            section_lines.append(
                f"func {name} {addr:04x} end:{next_addr:04x}")
        section_lines.append(INGEST_END)
        new_section = "\n".join(section_lines) + "\n"

        if cfg_path.exists():
            existing = hand_block.rstrip() + "\n\n"
            new_content = existing + new_section
        else:
            # cfg loader parses `bank = NN` via _parse_hex; emit in hex.
            new_content = (
                f"# bank{bank:02x}.cfg — auto-created by "
                f"ingest_sm_decomp.py\n\n"
                f"bank = {bank:02x}\n\n"
                f"{new_section}"
            )

        if dry_run:
            print(f"[dry-run] {cfg_path}: {len(filtered)} entries "
                  f"({len(dedup) - len(filtered)} suppressed)")
        else:
            cfg_path.write_text(new_content, encoding="utf-8")
            print(f"wrote {cfg_path}: {len(filtered)} entries "
                  f"({len(dedup) - len(filtered)} suppressed by hand-declared)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--decomp", default="F:/Projects/sm",
                    help="path to snesrev/sm decomp repo root")
    ap.add_argument("--output",
                    default="F:/Projects/snesrecomp/SuperMetroidRecomp/recomp",
                    help="path to recomp/ dir containing bank cfg files")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    decomp_root = Path(args.decomp)
    if not decomp_root.is_dir():
        print(f"--decomp not a directory: {decomp_root}", file=sys.stderr)
        return 1
    output_dir = Path(args.output)
    if not output_dir.is_dir():
        print(f"--output not a directory: {output_dir}", file=sys.stderr)
        return 1

    entries = harvest(decomp_root)
    print(f"harvested {len(entries)} (bank, pc16, name) tuples", file=sys.stderr)

    by_bank: dict[int, int] = defaultdict(int)
    for bank, _, _ in entries:
        by_bank[bank] += 1
    bank_hist = ", ".join(f"${b:02X}:{n}" for b, n in sorted(by_bank.items()))
    print(f"per-bank: {bank_hist}", file=sys.stderr)

    emit_per_bank(entries, output_dir, dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
