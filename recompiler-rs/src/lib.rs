//! Native whole-program analysis core for snesrecomp.
//!
//! The stable boundary is the format-3 `ProgramManifest` emitted by the
//! `snesrecomp-analyze` binary. Python remains the C-emission authority.

#![allow(clippy::too_many_arguments, clippy::type_complexity)]

pub mod cfg;
pub mod decoder;
pub mod insn;
pub mod rom;
