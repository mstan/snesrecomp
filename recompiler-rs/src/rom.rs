//! ROM loading + LoROM address mapping, reloc-aware (port of the ROM half of
//! `recompiler/snes65816.py`).
//!
//! Unlike the Python, there is no process-global reloc registry: reloc regions
//! are threaded explicitly (via `DecodeEnv` in later phases) and passed by `&`
//! to the byte-fetch translators here.

use std::io;
use std::path::Path;

/// Load a ROM image, stripping a 512-byte copier header if present.
pub fn load_rom<P: AsRef<Path>>(path: P) -> io::Result<Vec<u8>> {
    let data = std::fs::read(path)?;
    if data.len() % 1024 == 512 {
        Ok(data[512..].to_vec())
    } else {
        Ok(data)
    }
}

/// LoROM (bank, addr) -> physical ROM byte offset. `addr` must be in
/// $8000-$FFFF (the Python asserts this).
#[inline]
pub fn lorom_offset(bank: u32, addr: u32) -> usize {
    // Always assert (Python uses a bare `assert`, active in all builds) so an
    // invalid address fails loudly instead of underflowing in release.
    assert!(
        (0x8000..=0xFFFF).contains(&addr),
        "addr ${addr:04X} not in LoROM range $8000-$FFFF"
    );
    ((bank & 0x7F) as usize) * 0x8000 + (addr as usize - 0x8000)
}

/// A RAM-executed-from-ROM region: the bytes at WRAM `ram_addr..ram_addr+length`
/// (in `ram_bank`) are a linear copy of ROM `rom_off..` (in `rom_bank`). All
/// logical addresses stay at the WRAM execution address; only byte-fetch is
/// redirected to the ROM source. Mirrors the Python 5-tuple
/// `(ram_bank, ram_addr, rom_bank, rom_off, length)`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RelocRegion {
    pub ram_bank: u8,
    pub ram_addr: u16,
    pub rom_bank: u8,
    pub rom_off: u16,
    pub length: u32,
}

impl RelocRegion {
    pub fn new(ram_bank: u32, ram_addr: u32, rom_bank: u32, rom_off: u32, length: u32) -> Self {
        RelocRegion {
            ram_bank: (ram_bank & 0xFF) as u8,
            ram_addr: (ram_addr & 0xFFFF) as u16,
            rom_bank: (rom_bank & 0xFF) as u8,
            rom_off: (rom_off & 0xFFFF) as u16,
            length,
        }
    }
}

/// Return the matching reloc region for (bank, addr), or `None`. A match means
/// `addr` is in `[ram_addr, ram_addr+length)` for the given `ram_bank`.
pub fn addr_in_reloc_region(bank: u32, addr: u32, regions: &[RelocRegion]) -> Option<RelocRegion> {
    if regions.is_empty() {
        return None;
    }
    let bank = (bank & 0xFF) as u8;
    let addr = addr & 0xFFFF;
    for r in regions {
        if r.ram_bank != bank {
            continue;
        }
        let base = r.ram_addr as u32;
        if base <= addr && addr < base + r.length {
            return Some(*r);
        }
    }
    None
}

/// Map (bank, addr) to a physical ROM byte offset, reloc-aware. If (bank, addr)
/// is inside a registered reloc region, return the ROM offset of the source
/// byte; otherwise fall back to plain `lorom_offset`.
pub fn addr_to_rom_offset(bank: u32, addr: u32, regions: &[RelocRegion]) -> usize {
    if let Some(r) = addr_in_reloc_region(bank, addr, regions) {
        let delta = (addr & 0xFFFF) - (r.ram_addr as u32);
        return lorom_offset(r.rom_bank as u32, r.rom_off as u32) + delta as usize;
    }
    lorom_offset(bank, addr)
}

/// Borrow a `length`-byte slice of the ROM at LoROM (bank, addr).
pub fn rom_slice(rom: &[u8], bank: u32, addr: u32, length: usize) -> &[u8] {
    let off = lorom_offset(bank, addr);
    let end = (off + length).min(rom.len());
    &rom[off.min(rom.len())..end]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lorom_basic() {
        // bank 0, $8000 -> offset 0
        assert_eq!(lorom_offset(0x00, 0x8000), 0);
        // bank 1, $8000 -> 0x8000
        assert_eq!(lorom_offset(0x01, 0x8000), 0x8000);
        // bank&0x7F masks high bit
        assert_eq!(lorom_offset(0x80, 0x8000), 0);
        // addr offset within bank
        assert_eq!(lorom_offset(0x00, 0xD127), 0xD127 - 0x8000);
    }

    #[test]
    fn reloc_redirect() {
        // Star Fox: $7E:321F <- ROM $02:8000, len 0x5C00-ish.
        let regions = vec![RelocRegion::new(0x7E, 0x321F, 0x02, 0x8000, 0x6000)];
        // Inside the region: $7E:321F maps to ROM offset of $02:8000.
        assert_eq!(
            addr_to_rom_offset(0x7E, 0x321F, &regions),
            lorom_offset(0x02, 0x8000)
        );
        // Offset 0x10 in: maps 0x10 past the ROM base.
        assert_eq!(
            addr_to_rom_offset(0x7E, 0x322F, &regions),
            lorom_offset(0x02, 0x8000) + 0x10
        );
        // Outside the region (different bank) falls back to lorom_offset.
        assert!(addr_in_reloc_region(0x00, 0x8000, &regions).is_none());
    }
}
