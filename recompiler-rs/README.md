# Native whole-program analyzer

This crate accelerates the hot analysis phase of snesrecomp's v2 static
recompiler. It emits the stable format-3 `ProgramManifest`; the established
Python emitter consumes that manifest and remains responsible for generated C.

The native analyzer is the default when its release executable is present.
`tools/v2_emit.py --analysis-backend auto` falls back to Python if it is absent
or rejects an input. Use `--analysis-backend native` in CI when fallback would
hide a compatibility regression.

## Build

Install a current stable Rust toolchain, then run from the repository root:

```sh
python tools/build_native_analyzer.py --test
```

The helper performs locked release builds and produces:

- Windows: `recompiler-rs/target/release/snesrecomp-analyze.exe`
- Linux/macOS: `recompiler-rs/target/release/snesrecomp-analyze`

CI builds and tests Windows x86-64, Linux x86-64, and macOS arm64 executables.
Each workflow run publishes downloadable archives; published GitHub releases
receive the same archives as release assets. A downloaded binary can live
anywhere when `SNESRECOMP_NATIVE_ANALYZER` points to it.

## Use

Normal generation needs no extra option after the binary is built:

```sh
python tools/v2_emit.py --rom game.sfc --cfg-dir recomp \
  --out-dir src/gen --cfg-roots
```

Useful rollout modes:

```sh
# Require native analysis; fail instead of falling back.
python tools/v2_emit.py ... --analysis-backend native

# Force the reference implementation.
python tools/v2_emit.py ... --analysis-backend python
```

The native path currently supports the default `--max-insns=4096` and
`--max-nodes=100000` limits. Non-default limits select Python in `auto` mode.

## Correctness and performance

Both implementations share the manifest contract. CI compares that contract
on a synthetic pointer-call fixture, and `tools/v2_compare_analysis.py` compares
manifests from full game repositories. Use `--strict-summaries` to additionally
audit backend-local diagnostic graph details.

On the 2026-07-18 Mega Man X static-coverage workload, the Python analysis took
402.542 seconds and the native analysis took 14.599-15.693 seconds: 25.7-27.6x
faster. Both produced 4,561 variants, 4,032 exact exit facts, 558 exit-mode sets,
and the same 4,551/10 AOT/LLE split. Driving the Python emitter with either
manifest produced byte-identical generated C.

After pruning the historical full-emitter code from the production crate, a
cleaned release run on the same machine took 9.446 seconds (42.6x); the complete
native-analysis plus Python-emission command took 33.987 seconds and a verified
cache hit took 0.641 seconds.

The native parser/decoder implements the `indirect_dispatch ... ptrcall
targets:...` form used by Super Metroid-style pointer calls, including explicit
16/24-bit targets and PEA-derived return continuations. Full Super Metroid
analysis now completes in about 3.4 seconds, but its manifest is not yet
contract-equivalent (8,263 native variants versus 8,370 from Python). Therefore
`auto` deliberately falls back to Python when a cfg contains `ptrcall`.
Developers doing parity work can combine `--analysis-backend native` with the
hidden `--allow-experimental-native-features` switch; generated output from
that combination is not a production-supported path yet.

## Why Python remains

Python is deliberately retained for the first production rollout:

- it is the C emitter and compatibility oracle;
- `auto` mode keeps existing game repositories working without Rust;
- differential manifests make native regressions observable;
- removing it would provide little additional speed because analysis, not C
  emission, was the dominant cost.

Retire the Python analyzer only after supported game repositories pass required
native mode in CI for a release cycle. Replacing the Python emitter is a
separate project and should require generated-C and gameplay equivalence gates.
