# PSXRecomp screen-color models

`runner/src/snes/color_lut.c` adapts the screen-color lookup-table models from
`mstan/psxrecomp` revision
`d7815862e18ef939e5e6e5c6947f8c29667982d5`, pinned by
`mstan/MegaManX6Recomp` when inspected on 2026-07-21. The relevant
`runtime/src/color_lut.c` and `runtime/include/color_lut.h` files were
byte-identical at PSXRecomp revision
`d2006e02a3001495b1eedf2c1cc965d23c0de38f`, pinned by
`mstan/Tomba2Recomp` at that time.

The C color-science implementation derives from JRickey/gba-recomp
`crates/screen/src/{color,profile,lut}.rs`; the inspected upstream revision was
`de4edf59b872d887046d6a3b005e2df551b6d44c`. That lineage is licensed MIT OR
Apache-2.0. PSXRecomp's C adaptation and screen-model parameters are licensed
under PolyForm Noncommercial 1.0.0. Complete applicable license texts are in
this directory.

Local adaptations add SNES naming, a programmatic model-selection API,
environment compatibility, explicit invalid-model reporting, and conversion
from the SNES runner's completed `0x00RRGGBB` frame. The colorimetric constants
and Raw/CRT/Composite/Trinitron model math remain aligned with the cited
PSXRecomp implementation. Raw is an exact presentation bypass.

