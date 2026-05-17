"""Harvest function-definition PC comments from the snesrev/zelda3 decomp
and emit them as `name <pc6> <FunctionName>` cfg lines per bank.

The decomp at https://github.com/snesrev/zelda3 annotates every function
definition with its original SNES PC in a trailing comment, e.g.:

    void Interrupt_NMI(uint16 joypad_input) {  // 8080c9

This script extracts those (name, pc) pairs and emits them into the
per-bank cfg files of a snesrecomp-based project. Cfg `name` lines that
land in the matching bank are auto-promoted to function entries by the
v2 cfg loader (`cfg_loader.py:280`), giving static reachability a much
larger set of seeds to crawl.

Idempotent: each cfg file's auto-ingested section is delimited and
replaced wholesale on every run.

LoROM bank mirror: zelda3's PC comments use the `$80-$FF` half of the
mirror (e.g., `8080c9` = `$80:80C9`); for cfg purposes these are
normalised to the `$00-$7F` physical-bank form (`0080c9` = `$00:80C9`).

Usage:
    python tools/ingest_zelda3_decomp.py
        [--decomp F:/Projects/zelda3]
        [--output F:/Projects/LegendofZeldaAlttpRecomp/recomp]
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
#   <return-type>[*] <Name>(<args>) {  // <pc6>
# - return type: one identifier (optionally with leading `static`/`extern`)
# - args: any text, single line (multi-line signatures are not matched —
#   the decomp keeps signatures on one line in practice)
DEFN_RE = re.compile(
    r'^\s*'
    r'(?:static\s+|extern\s+)*'                    # optional storage class
    r'[A-Za-z_][\w]*'                              # return type word
    r'[\s\*]+'                                     # whitespace and/or pointer stars
    r'([A-Za-z_]\w*)'                              # function name (group 1)
    r'\s*\([^)]*\)\s*\{'                           # ( args ) {
    r'\s*//\s*'                                    # // comment leader
    r'([0-9a-fA-F]{6})'                            # pc6 (group 2)
    r'\s*$'                                        # nothing else after
)

INGEST_BEGIN = (
    "# >>> AUTO-INGESTED FROM zelda3 DECOMP "
    "— do not hand-edit between markers >>>"
)
INGEST_END = "# <<< END AUTO-INGESTED <<<"

# Skip dirs that contain no SNES function definitions.
_SKIP_DIR_PARTS = {"assets", "other", ".git", ".github", "build", "saves"}


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

    for bank in sorted(by_bank):
        items = sorted(by_bank[bank])
        # Dedupe by addr (a single PC may map to one canonical name; the
        # decomp's per-file order keeps the first encountered).
        seen = set()
        dedup: List[Tuple[int, str]] = []
        for addr, name in items:
            if addr in seen:
                continue
            seen.add(addr)
            dedup.append((addr, name))

        section_lines = [
            INGEST_BEGIN,
            "# Source: zelda3 decomp PC comments.",
            "# Regenerate via: python tools/ingest_zelda3_decomp.py",
            f"# {len(dedup)} entries.",
        ]
        for addr, name in dedup:
            section_lines.append(f"name {bank:02x}{addr:04x} {name}")
        section_lines.append(INGEST_END)
        new_section = "\n".join(section_lines) + "\n"

        cfg_path = output_dir / f"bank{bank:02x}.cfg"
        if cfg_path.exists():
            existing = cfg_path.read_text(encoding="utf-8")
            existing = ingest_section_re.sub("", existing)
            existing = existing.rstrip() + "\n\n"
            new_content = existing + new_section
        else:
            # NOTE: cfg loader parses `bank = NN` via _parse_hex, so emit
            # the bank field in hex (zero-padded). Plain `{bank}` would
            # produce decimal for $0A+ and the loader would mis-parse.
            new_content = (
                f"# bank{bank:02x}.cfg — auto-created by "
                f"ingest_zelda3_decomp.py\n\n"
                f"bank = {bank:02x}\n\n"
                f"{new_section}"
            )

        if dry_run:
            print(f"[dry-run] {cfg_path}: {len(dedup)} entries")
        else:
            cfg_path.write_text(new_content, encoding="utf-8")
            print(f"wrote {cfg_path}: {len(dedup)} entries")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--decomp", default="F:/Projects/zelda3",
                    help="path to zelda3 decomp repo root")
    ap.add_argument("--output",
                    default="F:/Projects/LegendofZeldaAlttpRecomp/recomp",
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

    # Quick per-bank histogram for sanity.
    by_bank: dict[int, int] = defaultdict(int)
    for bank, _, _ in entries:
        by_bank[bank] += 1
    bank_hist = ", ".join(f"${b:02X}:{n}" for b, n in sorted(by_bank.items()))
    print(f"per-bank: {bank_hist}", file=sys.stderr)

    emit_per_bank(entries, output_dir, dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
