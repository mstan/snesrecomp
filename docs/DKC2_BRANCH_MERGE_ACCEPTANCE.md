# DKC2 Branch Merge Acceptance

## Candidate

The starting point and merge candidate is the snesrecomp branch
`codex/dkc2-hirom-static-coverage` (draft PR #4). The purpose of this branch is
to make DKC2 work through faithful static recompilation without regressing the
existing SNES consumers.

`main` may be used as a diagnostic baseline only. Replacing, abandoning, or
substantially reverting the DKC2 work is not an acceptable regression fix.
Cross-game validation must consume this same candidate branch from isolated
worktrees; it must not modify the other games' main branches.

## Required five-game matrix

The branch is not merge-ready until all five games pass on the same engine
revision:

| Game | Required result |
| --- | --- |
| Donkey Kong Country 2 | DKC2 static-coverage behavior remains working |
| Super Mario World | No hangs, crashes, visual corruption, or garbled audio |
| The Legend of Zelda: A Link to the Past | No hangs, crashes, visual corruption, or garbled audio |
| Mega Man X | No hangs, crashes, visual corruption, or garbled audio |
| Super Metroid | No hangs, crashes, visual corruption, or garbled audio |

Every title requires both:

1. Automated validation: clean generation/build, deterministic attract soak,
   inspected framebuffer captures, structured runtime/state checks, and audio
   capture checks.
2. User validation: an intentional visible launch and brief gameplay test after
   the automated preflight has passed.

Frame advancement, readable WRAM, successful compilation, or process survival
alone do not constitute a pass. Unknown and untested are not pass states.

## Fix and evidence rules

- Use the original program/disassembly and interpreter or independent emulator
  as appropriate oracles. Generated C is evidence, not authority.
- Find the first deterministic divergence and fix the recompiler/runtime class;
  do not edit generated C or hide engine bugs with per-game CFG/HLE workarounds.
- Use structured, bounded debug surfaces rather than print-log instrumentation.
- Capture and inspect a screenshot before asserting visible state.
- Validate the harness itself for deterministic replay and fault detection
  before trusting cross-backend comparisons.
- Rerun the complete five-game matrix after every shared-engine correctness fix.
- Keep ROMs, save states, screenshots, audio captures, and other copyrighted or
  private test artifacts out of public commits.

## Current truthful status

- DKC2: automated frame/WRAM/VRAM regression gate and two-cycle attract soak
  pass on the candidate; final manual validation remains pending.
- SMW: the black-boot regression is fixed at its first deterministic
  checkpoint; longer attract/audio and manual validation remain pending.
- LttP: confirmed indoor-to-overworld transition regression; failed.
- MMX: built but not yet genuinely validated; unknown.
- Super Metroid: built but not yet genuinely validated; unknown.

Draft PR #4 remains a draft until every row passes both automated and user
validation and there are no known hangs, crashes, visual bugs, or garbled audio
in the exercised coverage.
