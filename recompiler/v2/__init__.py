"""snesrecomp.recompiler.v2

Correctness-first recompilation pipeline. See plan at
docs/correctness-pipeline.md (or the parsed-skipping-rainbow plan note).

Phase 1 (this commit): mode-state decoder graph keyed by (PC, M, X).
Later phases: CFG, IR, codegen on top.

Coexists with the v1 pipeline (recompiler/recomp.py et al.) until the
phase-7 merge gate flips the switch.
"""
