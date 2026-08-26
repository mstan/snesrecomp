#!/usr/bin/env python3
"""Probe a SNES ROM for the identity a new project needs.

This is the SNES counterpart to psxrecomp's ``probe_disc.py``. Where that tool
reads a Redump cue and pulls out a boot EXE, this one reads the cartridge
header and pulls out the mapping, title, region, and reset vector — the facts
the scaffold has to bake into game config, the seed bank config, CI, and the
README so nobody has to look them up by hand.

Everything printed here is derived from the ROM the user supplies; no ROM
bytes are copied into the project.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
for candidate in (ROOT, ROOT / "recompiler", ROOT / "tools"):
    value = str(candidate)
    if value not in sys.path:
        sys.path.insert(0, value)

try:
    from snes65816 import detect_rom_mapping, load_rom
except ImportError:  # pragma: no cover - only when run outside the repo
    detect_rom_mapping = None
    load_rom = None

# Cartridge header offsets, relative to the header base ($FFC0 in LoROM's
# bank $00 image, $FFC0 + $8000 for HiROM). Names follow the standard layout.
HEADER_TITLE = 0x00
HEADER_TITLE_LEN = 21
HEADER_MAP_MODE = 0x15
HEADER_CART_TYPE = 0x16
HEADER_ROM_SIZE = 0x17
HEADER_RAM_SIZE = 0x18
HEADER_REGION = 0x19
HEADER_CHECKSUM_COMP = 0x1C
HEADER_CHECKSUM = 0x1E
VECTOR_NATIVE_NMI = 0x2A
VECTOR_EMU_RESET = 0x3C

# Region byte -> (short code, human name). The short code drives packaging
# and catalog naming; the long name goes in the README.
REGIONS = {
    0x00: ("JPN", "Japan"),
    0x01: ("USA", "North America"),
    0x02: ("EUR", "Europe"),
    0x03: ("SWE", "Sweden / Scandinavia"),
    0x04: ("FIN", "Finland"),
    0x05: ("DNK", "Denmark"),
    0x06: ("FRA", "France"),
    0x07: ("NLD", "Netherlands"),
    0x08: ("ESP", "Spain"),
    0x09: ("DEU", "Germany"),
    0x0A: ("ITA", "Italy"),
    0x0B: ("CHN", "China"),
    0x0C: ("IDN", "Indonesia"),
    0x0D: ("KOR", "South Korea"),
    0x0F: ("CAN", "Canada"),
    0x10: ("BRA", "Brazil"),
    0x11: ("AUS", "Australia"),
}

# Cartridge-type byte -> coprocessor, for the ones this runner emulates.
# Anything absent is reported as unknown rather than guessed at: a project
# scaffolded for the wrong coprocessor fails in a confusing place later.
COPROCESSORS = {
    0x00: "none", 0x01: "none", 0x02: "none",
    0x03: "DSP", 0x04: "DSP", 0x05: "DSP",
    0x13: "SuperFX", 0x14: "SuperFX", 0x15: "SuperFX", 0x1A: "SuperFX",
    0x25: "OBC1",
    0x32: "SA-1", 0x34: "SA-1", 0x35: "SA-1", 0x36: "SA-1",
    0x43: "S-DD1", 0x45: "S-DD1",
    0x55: "S-RTC",
    0xE3: "Super Game Boy",
    0xF3: "CX4",
    0xF5: "SPC7110 / ST018",
    0xF6: "ST010 / ST011",
    0xF9: "SPC7110",
}


def header_base(raw: bytes, mapping: str) -> int:
    """Byte offset of the cartridge header for a de-headered image."""
    return 0x7FC0 if mapping == "lorom" else 0xFFC0


def read_u16(raw: bytes, offset: int) -> int:
    if offset + 1 >= len(raw):
        return 0
    return raw[offset] | (raw[offset + 1] << 8)


def clean_title(raw_title: bytes) -> str:
    text = raw_title.decode("ascii", errors="replace")
    text = "".join(ch if 0x20 <= ord(ch) < 0x7F else " " for ch in text)
    return " ".join(text.split())


def clean_filename_title(stem: str) -> str:
    """Filename stem with the usual dump tags removed.

    No-Intro / Redump names carry the real title, so
    "Shin Kidou Senki Gundam W - Endless Duel (Japan)" becomes
    "Shin Kidou Senki Gundam W - Endless Duel".
    """
    text = re.sub(r"[\(\[][^\)\]]*[\)\]]", " ", stem)
    text = text.replace("_", " ")
    return " ".join(text.split()).strip(" -")


def display_name_from(title: str, filename_stem: str) -> str:
    """The best human title available.

    The 21-byte header field is upper-case, padded, and often mangled
    ("GUNDAMW ENDLESSDUEL"), while a dump filename usually carries the real
    one. Prefer the filename when it looks like a title — more than one word,
    or hyphenated — and fall back to the header otherwise, so a terse
    "smw.sfc" still gets the header's name rather than "Smw". Either way the
    scaffolder offers this as a prompt default, not a decision.
    """
    from_file = clean_filename_title(filename_stem)
    looks_like_title = " " in from_file or "-" in from_file
    if from_file and looks_like_title:
        return from_file
    if not title:
        return from_file or filename_stem
    if title.isupper() and len(title) > 3:
        return title.title()
    return title


def safe_slug(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9]+", "", value.title())
    return cleaned or "SnesGame"


def project_name(display: str) -> str:
    """Repo / CMake project name: <Title>SNESRecomp, matching the workspace."""
    return safe_slug(display) + "SNESRecomp"


def probe(path: pathlib.Path) -> dict:
    raw_file = path.read_bytes()
    if load_rom is not None:
        normalized = bytes(load_rom(str(path)))
        mapping = detect_rom_mapping(normalized)
    else:  # pragma: no cover - degraded path outside the repo
        normalized = raw_file[512:] if len(raw_file) % 1024 == 512 else raw_file
        mapping = "lorom"

    base = header_base(normalized, mapping)
    title = clean_title(normalized[base + HEADER_TITLE:
                                   base + HEADER_TITLE + HEADER_TITLE_LEN])
    region_byte = normalized[base + HEADER_REGION] if base + HEADER_REGION < len(normalized) else 0xFF
    region_code, region_name = REGIONS.get(region_byte, ("UNK", "unknown"))
    cart_type = normalized[base + HEADER_CART_TYPE] if base + HEADER_CART_TYPE < len(normalized) else 0
    rom_size_byte = normalized[base + HEADER_ROM_SIZE] if base + HEADER_ROM_SIZE < len(normalized) else 0
    ram_size_byte = normalized[base + HEADER_RAM_SIZE] if base + HEADER_RAM_SIZE < len(normalized) else 0

    checksum = read_u16(normalized, base + HEADER_CHECKSUM)
    complement = read_u16(normalized, base + HEADER_CHECKSUM_COMP)
    reset_vector = read_u16(normalized, base + VECTOR_EMU_RESET)
    nmi_vector = read_u16(normalized, base + VECTOR_NATIVE_NMI)

    display = display_name_from(title, path.stem)
    return {
        "rom_file": path.name,
        "file_size": len(raw_file),
        "normalized_size": len(normalized),
        "had_copier_header": len(raw_file) != len(normalized),
        "crc32": "%08x" % (zlib.crc32(raw_file) & 0xFFFFFFFF),
        "sha256": hashlib.sha256(raw_file).hexdigest(),
        "mapping": mapping,
        "header_title": title,
        "file_title": clean_filename_title(path.stem),
        "display_name": display,
        "project_name": project_name(display),
        "region_byte": region_byte,
        "region": region_code,
        "region_name": region_name,
        "cart_type": cart_type,
        "coprocessor": COPROCESSORS.get(cart_type, "unknown"),
        "rom_size_kb": (1 << rom_size_byte) if rom_size_byte and rom_size_byte < 24 else 0,
        "sram_size_kb": (1 << ram_size_byte) if ram_size_byte and ram_size_byte < 24 else 0,
        "checksum": "%04x" % checksum,
        "checksum_complement": "%04x" % complement,
        "checksum_valid": (checksum ^ complement) == 0xFFFF,
        "reset_vector": "%04x" % reset_vector,
        "nmi_vector": "%04x" % nmi_vector,
    }


def seed_cfg(info: dict) -> str:
    """Starter bank $00 config.

    `auto_vectors` seeds analysis from the hardware vectors, which is the only
    entry point that is knowable without game-specific work. `tier_down_stubs`
    lets anything the analyzer cannot prove fall through to the interpreter
    instead of being synthesised — no stubs, per the workspace doctrine.
    """
    return f"""# {info['display_name']} ({info['region']}) — LLE-first seed
# Regenerate with tools/regen.sh. Promote functions by editing recomp/symbols.toml
# and re-running the regen script, which rewrites the block below.
bank = 0
auto_vectors
tier_down_stubs

# Reset vector ${info['reset_vector']}, native NMI ${info['nmi_vector']} (from the cartridge header).

# >>> BEGIN symbols.toml (generated — do not edit)
# <<< END symbols.toml
"""


def symbols_toml(info: dict) -> str:
    return f"""# Progressive symbol map for {info['display_name']}.
#
# Each [[func]] names an address the analyzer should treat as a function.
# Set emit = true to promote one into ahead-of-time codegen; leave it false
# to keep it interpreted. tools/regen.sh syncs this into recomp/bank00.cfg
# and recomp/funcs.h — do not hand-edit the generated blocks there.

[[func]]
name = "I_RESET"
addr = "{info['reset_vector']}"
bank = 0
emit = false
note = "Emulation RESET vector ($FFFC)"

[[func]]
name = "I_NMI"
addr = "{info['nmi_vector']}"
bank = 0
emit = false
note = "Native NMI vector ($FFEA)"
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rom", help="path to a .sfc or .smc ROM")
    parser.add_argument("--json-out", help="write the full probe as JSON")
    parser.add_argument("--write-seed-cfg", help="write a starter bank00.cfg here")
    parser.add_argument("--write-symbols", help="write a starter symbols.toml here")
    parser.add_argument("--display-name",
                        help="override the header-derived title, so a name "
                             "given on the command line reaches the seed "
                             "config and symbol map too")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    path = pathlib.Path(args.rom).expanduser().resolve()
    if not path.is_file():
        print(f"probe_rom: ROM not found: {path}", file=sys.stderr)
        return 1
    info = probe(path)
    if args.display_name:
        info["display_name"] = args.display_name
        info["project_name"] = project_name(args.display_name)

    if args.json_out:
        out = pathlib.Path(args.json_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    if args.write_seed_cfg:
        out = pathlib.Path(args.write_seed_cfg)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(seed_cfg(info), encoding="utf-8")
    if args.write_symbols:
        out = pathlib.Path(args.write_symbols)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(symbols_toml(info), encoding="utf-8")

    if not args.quiet:
        for key in ("display_name", "project_name", "mapping", "region",
                    "coprocessor", "rom_size_kb", "sram_size_kb",
                    "crc32", "sha256", "reset_vector", "checksum_valid"):
            print(f"{key}={info[key]}")
        if not info["checksum_valid"]:
            print("warning: header checksum does not verify — the image may be "
                  "modified, over-dumped, or not a plain SNES ROM",
                  file=sys.stderr)
        if info["coprocessor"] == "unknown":
            print(f"warning: unrecognised cartridge type ${info['cart_type']:02x} "
                  "— check coprocessor support before investing in this port",
                  file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
