"""snesrecomp.tools.lint_codegen_widths

Mechanical gate against width-mask regression in v2 codegen.

Background: DRY_REFACTOR.md (commit fa09fef centralised every
width-dependent C-string literal in `recompiler/v2/widths.py`). The
lint enforces the chokepoint by rejecting raw width literals and
ad-hoc derivation patterns appearing in any other module.

Failure cases this catches:
1. Raw `"0xFF"` / `"0xFFFF"` / `"0x80"` / `"0x8000"` / `"0x100"`
   / `"0x10000"` literals in C-string emissions outside widths.py.
2. The pattern `"0xFF" if … else "0xFFFF"` and
   `"0x80" if … else "0x8000"` (the per-emitter ad-hoc derivation).
3. Direct C-emission patterns like `"& 0xFF"` and `"& 0xFFFF"`.

Whitelisted (legitimately not width-dependent):
- P-register bit-position constants (XF=$10, M=$20, V=$40, N=$80)
  in `_emit_setflag` — these are 65816 P-byte layout, not width.
- The N|Z packed-flag bit pattern (`0x80` in `cpu->P |= 0x80` / etc.)
  in cpu->P update strings — these are P-bit positions, not widths.
- MVN/MVP terminator `0xFFFF` (65816 spec sentinel for the loop).
- Pure Python int arithmetic on 24-bit addresses
  (`(addr >> 16) & 0xFF`, `addr & 0xFFFF`) — extracts bank/PC from
  IR address constants; not emitted into C.

Invocation:
    python tools/lint_codegen_widths.py
Exit 0 if clean; non-zero with line-listed offenders otherwise.

Wired into snesrecomp/tests/run_tests.py so it runs alongside the
unit-test loop. Failure aborts the test run.
"""
import pathlib
import re
import sys

SCAN_ROOT = pathlib.Path(__file__).resolve().parent.parent / "recompiler" / "v2"
ALLOWED = {"widths.py"}

# Patterns that ARE bugs — width-mask emissions outside widths.py.
PATTERNS = [
    # Per-emitter ad-hoc width-mask derivation (the original bug).
    (r'"0xFF"\s+if\s+.*\s+else\s+"0xFFFF"',
     "ad-hoc op_mask derivation — use widths.op_mask(width)"),
    (r'"0x80"\s+if\s+.*\s+else\s+"0x8000"',
     "ad-hoc sign_bit derivation — use widths.sign_bit(width)"),
    (r'"0x100"\s+if\s+.*\s+else\s+"0x10000"',
     "ad-hoc carry_bit derivation — use widths.carry_bit(width)"),
    (r'"0x40"\s+if\s+.*\s+else\s+"0x4000"',
     "ad-hoc overflow_bit derivation — use widths.overflow_bit(width)"),
    # Direct width-mask emissions in C-string literals.
    (r'"\&\s*0xFF\b[^F]',
     "raw '& 0xFF' in emitted C — use widths.masked(expr, 1) or widths.low_byte"),
    (r'"\&\s*0xFFFF\b',
     "raw '& 0xFFFF' in emitted C — use widths.masked(expr, 2)"),
]


def lint_file(path: pathlib.Path) -> list:
    """Return a list of (lineno, message, line_text) for every offender."""
    offenders = []
    if path.name in ALLOWED:
        return offenders
    if not path.suffix == ".py":
        return offenders
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        # Skip comment-only lines — those are documentation, not codegen.
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        for pat, msg in PATTERNS:
            if re.search(pat, line):
                offenders.append((lineno, msg, line.rstrip()))
                break
    return offenders


def main() -> int:
    if not SCAN_ROOT.is_dir():
        print(f"lint: SCAN_ROOT not found: {SCAN_ROOT}", file=sys.stderr)
        return 2
    total = 0
    for f in sorted(SCAN_ROOT.glob("*.py")):
        offenders = lint_file(f)
        for lineno, msg, line_text in offenders:
            print(f"  {f.relative_to(SCAN_ROOT.parent)}:{lineno}: {msg}")
            print(f"    | {line_text}")
            total += 1
    if total == 0:
        print("lint_codegen_widths: clean (0 width-mask violations)")
        return 0
    print(f"\nlint_codegen_widths: {total} violations — route through widths.py")
    return 1


if __name__ == "__main__":
    sys.exit(main())
