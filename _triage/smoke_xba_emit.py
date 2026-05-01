"""Smoke-check the XBA emitter output post-fix."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'recompiler'))
from v2 import ir, codegen

op = ir.XBA()
print('\n'.join(codegen._emit_xba(op)))
