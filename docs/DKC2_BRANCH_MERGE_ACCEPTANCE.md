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

The runtime candidate tested below is `2b00d9f` (the following commits may be
documentation-only). Its Linux x86-64, Windows x86-64, and macOS arm64 CI jobs
pass. All generation in this matrix uses the native Rust analyzer.

| Game | Automated runtime/video | Automated audio | User gameplay |
| --- | --- | --- | --- |
| DKC2 | Pass: current-candidate frame/WRAM/VRAM preservation plus a 12,000-frame, two-attract-cycle state soak | Pass: the DKC2 integrity soak exercises the boot upload and sustained SPC run | Pass: previously checked by the user on this candidate line |
| SMW | Pass: Rust regeneration, 7,200-frame attract soak, zero dispatch misses, and inspected clean framebuffer | Pending: the prior manual launch was invalid for audio because a local, untracked test override disabled the SDL consumer; rerun required with the corrected validation configuration | In progress; current visible title/gameplay session is clean so far, but the user has not reported a final result |
| LttP | Pass: Rust regeneration, exact indoor-to-overworld save transition, 7,200-frame soak, zero dispatch misses, and inspected clean framebuffer | Pending explicit live-consumer/WAV integrity gate | Pending |
| MMX | Pass: Rust regeneration, 7,200-frame title/intro soak, zero dispatch misses, and inspected clean framebuffers | Pending explicit live-consumer/WAV integrity gate | Pending |
| Super Metroid | Pass: Rust regeneration, 7,200-frame title/cinematic soak, zero dispatch misses, and inspected clean framebuffers | Pending explicit live-consumer/WAV integrity gate | Pending |

The LttP transition regression was a class-level computed-RTS ownership bug in
the interpreter/AOT bridge. Commit `be4a706` fixes it and includes emitted-code
and bridge-runtime regressions. The no-deny save transition now reaches the
known-good state hash
`81d71b22fc929c218fa57335c6e5f5b7da50fb3182dd585cd018d11c15efa716`.

The remaining automated work is the explicit live audio-consumer gate for the
four non-DKC2 games. It requires active nonzero PCM, no clipping, no audible
ring drops, an active consumer, and populated faithful Gaussian/BRR/echo
reference measurements. Runtime/video success does not substitute for this
gate.

Draft PR #4 remains a draft until every row passes both automated and user
validation and there are no known hangs, crashes, visual bugs, or garbled audio
in the exercised coverage.
