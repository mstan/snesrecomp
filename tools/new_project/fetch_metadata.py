#!/usr/bin/env python3
"""Fetch a cartridge's publisher, developer and release year from libretro-database.

libretro-database keeps per-system metadata DATs keyed by the ROM's CRC32:

  metadat/publisher/<System>.dat     game ( comment "<name>" publisher "X" rom ( crc ... ) )
  metadat/developer/<System>.dat     ... developer "X" ...
  metadat/releaseyear/<System>.dat   ... releaseyear "1994" ...

There is no marketing description there (a DAT "description" is the name
again), so the wizard asks for one afterwards if nothing else supplies it.

Companion to fetch_boxart.py: same "Fetch boxart and metadata" step, same
system names. Network required; the DATs are cached for a week under
~/.cache/snesrecomp/libretro-database/. Exit 0 with JSON on stdout (or
--json-out) when at least one field was found, 1 when nothing was.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

LIBRETRO_RAW = "https://raw.githubusercontent.com/libretro/libretro-database/master/"
DEFAULT_SYSTEM = "Nintendo - Super Nintendo Entertainment System"
FIELDS = ("publisher", "developer", "releaseyear")
CACHE_MAX_AGE_SEC = 7 * 24 * 3600
USER_AGENT = "snesrecomp-new-project/1.0"


def cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "snesrecomp" / "libretro-database"


def http_get(url: str, timeout: float = 90.0) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def dat_text(field: str, system: str, *, force: bool = False) -> str:
    rel = f"metadat/{field}/{system}.dat"
    local = cache_dir() / rel
    if not force and local.is_file() and time.time() - local.stat().st_mtime < CACHE_MAX_AGE_SEC:
        return local.read_text(encoding="utf-8", errors="replace")
    data = http_get(LIBRETRO_RAW + urllib.parse.quote(rel))
    local.parent.mkdir(parents=True, exist_ok=True)
    local.write_bytes(data)
    return data.decode("utf-8", errors="replace")


_GAME_RE = re.compile(r"game \(\s*(.*?)\n\)", re.S)
_KV_RE = re.compile(r'^\s*(\w+)\s+"((?:[^"\\]|\\.)*)"', re.M)
_CRC_RE = re.compile(r"rom \([^)]*\bcrc\s+([0-9A-Fa-f]{8})", re.S)


def find_by_crc(text: str, crc32: str) -> dict[str, str] | None:
    want = crc32.strip().lower().lstrip("0x").zfill(8)
    for m in _GAME_RE.finditer(text):
        block = m.group(1)
        c = _CRC_RE.search(block)
        if not c or c.group(1).lower() != want:
            continue
        return {k: v for k, v in _KV_RE.findall(block)}
    return None


def lookup(crc32: str, system: str = DEFAULT_SYSTEM, *, force: bool = False) -> dict[str, str]:
    out: dict[str, str] = {}
    for field in FIELDS:
        try:
            text = dat_text(field, system, force=force)
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
            print(f"warning: {field} DAT unavailable: {exc}", file=sys.stderr)
            continue
        hit = find_by_crc(text, crc32)
        if not hit:
            continue
        if hit.get("comment") and not out.get("name"):
            out["name"] = hit["comment"]
        if hit.get(field):
            out["year" if field == "releaseyear" else field] = hit[field]
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--crc32", required=True, help="ROM CRC32, 8 hex digits")
    ap.add_argument("--system", default=DEFAULT_SYSTEM,
                    help="libretro-database system name")
    ap.add_argument("--json-out", default="", help="write the result here too")
    ap.add_argument("--refresh", action="store_true", help="ignore the cache")
    args = ap.parse_args()
    if not re.fullmatch(r"[0-9A-Fa-f]{8}", args.crc32.strip()):
        print(f"error: --crc32 must be 8 hex digits, got {args.crc32!r}", file=sys.stderr)
        return 2
    hit = lookup(args.crc32, args.system, force=args.refresh)
    hit["source"] = LIBRETRO_RAW + "metadat/ (publisher, developer, releaseyear DATs, by CRC32)"
    text = json.dumps(hit, indent=2)
    if args.json_out:
        Path(args.json_out).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if any(k in hit for k in ("publisher", "developer", "year")) else 1


if __name__ == "__main__":
    raise SystemExit(main())
