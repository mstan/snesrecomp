# Third-Party Attribution

snesrecomp incorporates the following third-party software.

## Native analyzer foundation

The Rust instruction decoder, cfg parser, and ROM mapping foundation under
`recompiler-rs/` originated in Colin Curtin's `perplexes/snesrecomp`
`feat/superfx-gsu/recompiler-rs` work and has since been reduced to the native
analysis boundary, updated for the current Python semantics, and extended with
the whole-program fixed point and production integration.

- Upstream: https://github.com/perplexes/snesrecomp
- Original branch: `feat/superfx-gsu/recompiler-rs`
- License declared by the upstream crate: MIT

## LakeSnes — 65816 CPU core

`runner/src/snes/interp816.{c,h}`, the 65816 interpreter backing the
interpreter-fallback tier (see `docs/MULTI_TIER.md`), is derived from the CPU
core of **LakeSnes** by angelo_wf.

- Upstream: https://github.com/angelo-wf/lakesnes
- License: MIT

### Derivation / modifications

Recovered from `runner/src/snes/cpu.c` as it existed at commit `9de9855^` —
the snesrecomp tree's original LakeSnes adaptation, before the unused
interpreter was ripped on 2026-04-20 — then re-vendored for the
interpreter-fallback tier with:

- symbols namespaced `cpu_*` / `Cpu` → `interp816_*` / `Interp816` so the core
  coexists with the legacy `Cpu` debug shadow (`runner/src/snes/cpu.{c,h}`);
- the hardwired `snes_cpuRead` / `snes_cpuWrite` bus replaced with a
  caller-supplied callback bus (so the production adapter can route memory
  through the AOT `cpu_read8` / `cpu_write8` HLE bus);
- snesrecomp debug instrumentation removed (`pc_hist` / `DumpCpuHistory` and
  the top-of-`doOpcode` assert tripwire);
- `WAI` restored to stock behavior (`waiting = true`, was an assert);
- `BRK` routed to the `interp816_opcode_hook` bridge seam.

The exact transform is reproducible: `git show 9de9855^:runner/src/snes/cpu.c`,
then the renames + seam edits described above. The vendored core is validated
by `tests/interp816/` (directed opcode harness).

### MIT License

```
Copyright (c) 2021-2023 angelo_wf and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
