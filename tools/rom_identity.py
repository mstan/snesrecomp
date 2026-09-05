#!/usr/bin/env python3
"""rom_identity.py — read a project's rom_identity.txt.

The build generates snesrecomp_rom_identity.h from that file via CMake
(snesrecomp_rom_identity() in runner/runner.cmake). CI cannot use that path:
a scaffolded project refuses to configure until src/gen/ has been generated
from a ROM, and CI has no ROM. So this emits the identical header without a
configure, letting CI syntax-check the host translation units.

The values still live in exactly one file. What is duplicated is the parser,
not the data -- and `tests/test_rom_identity.py` pins this emitter and the
CMake one to the same output so they cannot drift.

    python3 tools/rom_identity.py rom_identity.txt --header out/dir
    python3 tools/rom_identity.py rom_identity.txt --get expected_sha256
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

FIELDS = ("display_name", "rom_file", "expected_crc32",
          "expected_sha256", "mapping", "region")

LINE = re.compile(r"^([A-Za-z0-9_]+)[ \t]*=[ \t]*(.*)$")

HEADER = '''/* Generated from {source} by snesrecomp_rom_identity().
 * Do not edit: edit that file and rebuild.
 *
 * The digests are compiled in rather than read at runtime so a shipped build
 * carries no extra file it can be separated from. The text file stays the one
 * place a human edits, and tools/regen.sh and CI read the same file, so a
 * revision change cannot land in the binary and miss the tooling.
 *
 * Any field absent from the identity file expands to an empty string, which
 * every consumer reads as "cannot verify" rather than "verified". */
#ifndef SNESRECOMP_ROM_IDENTITY_H
#define SNESRECOMP_ROM_IDENTITY_H

#define SNESRECOMP_ROM_DISPLAY_NAME    "{display_name}"
#define SNESRECOMP_ROM_FILE            "{rom_file}"
#define SNESRECOMP_ROM_EXPECTED_CRC32  "{expected_crc32}"
#define SNESRECOMP_ROM_EXPECTED_SHA256 "{expected_sha256}"
#define SNESRECOMP_ROM_MAPPING         "{mapping}"
#define SNESRECOMP_ROM_REGION          "{region}"

#endif /* SNESRECOMP_ROM_IDENTITY_H */
'''


def parse(path: pathlib.Path) -> dict[str, str]:
    """key -> value. Unknown keys are kept; missing ones default to ''."""
    out = {f: "" for f in FIELDS}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = LINE.match(line)
        if not m:
            print(f"{path}: ignoring unparsable line: {line}", file=sys.stderr)
            continue
        value = m.group(2).strip()
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
            value = value[1:-1]
        out[m.group(1).lower()] = value
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("identity", type=pathlib.Path)
    ap.add_argument("--header", type=pathlib.Path,
                    help="directory to write snesrecomp_rom_identity.h into")
    ap.add_argument("--get", help="print one field and exit")
    args = ap.parse_args()

    if not args.identity.is_file():
        print(f"no such file: {args.identity}", file=sys.stderr)
        return 2

    ident = parse(args.identity)

    if args.get:
        print(ident.get(args.get.lower(), ""))
        return 0

    if not args.header:
        ap.error("one of --header or --get is required")

    if not ident["expected_sha256"] and not ident["expected_crc32"]:
        print(f"{args.identity}: sets neither expected_sha256 nor "
              f"expected_crc32 — this build cannot verify the ROM it is "
              f"handed.", file=sys.stderr)

    args.header.mkdir(parents=True, exist_ok=True)
    dst = args.header / "snesrecomp_rom_identity.h"
    dst.write_text(HEADER.format(source=args.identity, **ident),
                   encoding="utf-8")
    print(dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
