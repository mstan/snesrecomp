# interp816 validation

Directed-opcode harness that proves the vendored LakeSnes-derived 65816 core
(`runner/src/snes/interp816.{c,h}`) is semantically intact after the rename +
de-cruft vendoring. This is the Phase-0 gate for the interpreter-fallback tier
described in `docs/MULTI_TIER.md`.

It is a standalone C harness (the rest of `tests/` is the Python regen suite),
built with gcc over a flat-RAM bus — no game, no ROM, no full runner.

## Run

```sh
tests/interp816/run.sh        # builds + runs both harnesses
```

(Build via WSL on Windows; validation only — not part of the game build.)

## Coverage

**`interp816_test.c` (Phase 0 — core, 17 checks):** 8-bit `LDA` + Z/N flags; the
emulation→native `CLC;XCE` + `REP #$30` width switch to 16-bit; 16-bit
`LDA`/`STA` absolute store+reload via the bus; 8-bit `INX` wrap; binary and
**decimal** `ADC`; `PHA`/`PLA`; `TAX`; `XBA`; a taken `BNE`.

**`bridge_test.c` (Phase 1 — interp↔AOT bridge contract, 12 checks):** with a
fake bus + a single fake "compiled" entry (mutates A, models its RTS):
- S1 — JSR into a compiled entry is **bounced** (`cpu_dispatch_pc`), the
  compiled body runs, register state syncs both ways, the stack stays balanced,
  and interpretation resumes at the return address;
- S2 — a pure interpreted routine exits balanced with no bounce;
- S3 — a JSR to a **non**-compiled target is interpreted through; the nested
  RTS returns to caller level without prematurely exiting; the final RTS exits
  balanced.

## Not yet covered (Phase 1b)

Wiring the bridge to a real production trap site (`dispatch_oob` / bank-miss) in
the game build, and on-hardware playtest. That step needs the full runner + a
ROM and is validated by the project owner.
