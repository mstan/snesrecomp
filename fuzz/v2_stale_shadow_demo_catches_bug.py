"""Prove v2_stale_shadow.py would have caught the original cpu->B bug.

Monkey-patches v2.codegen._emit_xba to its pre-6c04c94 buggy form (the
one that read stale cpu->B as the new A.low) and patches the harness's
CpuState definition to include a `B` field initialised from the seed's
high byte. Runs the snippet set; expects failures matching the SMW
class.

Restores nothing — this script doesn't modify the repo, only its own
in-process imports. Re-running v2_stale_shadow.py after this still uses
the fixed emit and passes."""
from __future__ import annotations
import re
import subprocess
import sys
import pathlib

FUZZ = pathlib.Path(__file__).resolve().parent
REPO = FUZZ.parent
sys.path.insert(0, str(REPO / 'recompiler'))

# Import the live module so we can patch it.
from v2 import codegen
from v2.ir import XBA


# Buggy XBA emitter — copy of the pre-6c04c94 implementation that reads
# the stale cpu->B shadow as the new A.low.
def _emit_xba_buggy(op):
    return [
        "{",
        "  uint8 _lo = (uint8)(cpu->A & 0xFF);",
        "  cpu->A = (uint16)((uint16)cpu->B | ((uint16)_lo << 8));",
        "  cpu->B = (uint8)((cpu->A >> 8) & 0xFF);",
        "  cpu->_flag_Z = (((cpu->A & 0xFF)) == 0) ? 1 : 0;",
        "  cpu->_flag_N = ((((cpu->A & 0xFF)) & 0x80) != 0) ? 1 : 0;",
        "}",
    ]


# Monkey-patch the dispatch table. _OP_HANDLERS in codegen maps op
# classes to emitter functions. Find the entry for XBA and override.
codegen._DISPATCH[XBA] = _emit_xba_buggy

# Now import the fuzz harness module — which calls codegen.emit_op at
# render time, so it'll pick up our patched handler.
import v2_stale_shadow as harness  # noqa: E402

# Inject a `B` field into the harness's CpuState struct + init line. We
# do this by running v2_stale_shadow.main() with patched constants. To
# avoid maintaining a parallel harness, monkey-patch the relevant pieces:

_original_render = harness.render_run_all
_original_prologue = harness.C_HARNESS_PROLOGUE


def patched_prologue() -> str:
    # Add `uint8 B` field after `uint16 A`.
    return _original_prologue.replace(
        "    uint16 A;\n",
        "    uint16 A;\n    uint8  B;  /* legacy stale shadow — buggy emit only */\n",
    )


def patched_render(snippets):
    out = _original_render(snippets)
    # Inject `cpu.B = <high byte of seed.A>` after `cpu.A = ...`.
    def add_b_init(match):
        line = match.group(0)
        # Extract A value
        m = re.search(r'cpu\.A = 0x([0-9a-fA-F]+);', line)
        if not m:
            return line
        a_val = int(m.group(1), 16)
        b_val = (a_val >> 8) & 0xFF
        return line + f'\n    cpu.B = 0x{b_val:02x};'
    out = re.sub(r'    cpu\.A = 0x[0-9a-fA-F]+;', add_b_init, out)
    return out


harness.C_HARNESS_PROLOGUE = patched_prologue()
harness.render_run_all = patched_render

print('Running v2 stale-shadow fuzz against BUGGY XBA emit...\n')
rc = harness.main()
print()
if rc != 0:
    print('-> Fuzz correctly caught the bug. Score: works as designed.')
    sys.exit(0)
else:
    print('-> ALL passed?? The fuzz did NOT catch the bug. Test gap.')
    sys.exit(1)
