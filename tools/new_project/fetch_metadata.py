#!/usr/bin/env python3
"""Fetch a cartridge's name, publisher, developer, year and description by its CRC32.

Everything starts from the ROM's digest, never from a guessed title:

  metadat/no-intro/<System>.dat      game ( name "<No-Intro name>" rom ( crc ... ) )
  metadat/publisher/<System>.dat     game ( comment "<name>" publisher "X" rom ( crc ... ) )
  metadat/developer/<System>.dat     ... developer "X" ...
  metadat/releaseyear/<System>.dat   ... releaseyear "1994" ...

The No-Intro name is what libretro's thumbnails are filed under, so a boxart
fetch by that exact name lands first time. libretro carries no marketing
description; that comes from Wikipedia's REST summary for the title (the
No-Intro name with its region tags removed), accepted only when the page is
a plain article about a game. The extract is CC BY-SA; the source URL is
returned with it so the project can attribute it.

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
WIKIPEDIA_SUMMARY = "https://en.wikipedia.org/api/rest_v1/page/summary/"
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
# A No-Intro rom line quotes a name that itself contains parentheses --
# rom ( name "Super Metroid (Japan, USA) (En,Ja).sfc" size ... crc ... ) --
# so the scan must step over quoted strings rather than stop at the first ')'.
_CRC_RE = re.compile(r'rom \((?:[^()"]|"[^"]*")*?\bcrc\s+([0-9A-Fa-f]{8})', re.S)


def find_by_crc(text: str, crc32: str) -> dict[str, str] | None:
    want = crc32.strip().lower().lstrip("0x").zfill(8)
    for m in _GAME_RE.finditer(text):
        block = m.group(1)
        c = _CRC_RE.search(block)
        if not c or c.group(1).lower() != want:
            continue
        return {k: v for k, v in _KV_RE.findall(block)}
    return None


def nointro_name(crc32: str, system: str, *, force: bool = False) -> str:
    """The No-Intro name for this dump, or ""."""
    try:
        text = dat_text("no-intro", system, force=force)
    except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
        print(f"warning: no-intro DAT unavailable: {exc}", file=sys.stderr)
        return ""
    hit = find_by_crc(text, crc32)
    return (hit or {}).get("name", "")


def title_from_name(name: str) -> str:
    """'Super Metroid (Japan, USA) (En,Ja)' -> 'Super Metroid'."""
    return re.sub(r"\s*\([^)]*\)", "", name).strip()


def wikipedia_description(title: str) -> tuple[str, str]:
    """(short description, source URL) from Wikipedia's page summary, or ("", "").

    Tries '<title> (video game)' before '<title>', and takes a page only when
    it is a plain article whose summary is about a game -- a title that is
    also a film or a band must not come back with the wrong page's text.
    """
    if not title:
        return "", ""
    for cand in (f"{title} (video game)", f"{title} (SNES video game)", title):
        url = WIKIPEDIA_SUMMARY + urllib.parse.quote(cand.replace(" ", "_"))
        try:
            data = json.loads(http_get(url, timeout=30).decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, OSError, ValueError):
            continue
        if data.get("type") != "standard":
            continue
        short = (data.get("description") or "").lower()
        extract = (data.get("extract") or "").strip()
        if "game" not in short and "game" not in extract[:200].lower():
            continue
        # The first two sentences make a README-sized blurb.
        sentences = re.split(r"(?<=[.!?])\s+", extract)
        blurb = " ".join(sentences[:2]).strip()
        if len(blurb) > 400:
            blurb = sentences[0].strip()
        page = (data.get("content_urls") or {}).get("desktop", {}).get("page") or url
        return blurb, page
    return "", ""


def lookup(crc32: str, system: str = DEFAULT_SYSTEM, *, force: bool = False,
           description: bool = True) -> dict[str, str]:
    out: dict[str, str] = {}
    ni = nointro_name(crc32, system, force=force)
    if ni:
        out["nointro_name"] = ni
        out["name"] = ni
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
    if description and out.get("name"):
        blurb, source = wikipedia_description(title_from_name(out["name"]))
        if blurb:
            out["description"] = blurb
            out["description_source"] = source
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--crc32", required=True, help="ROM CRC32, 8 hex digits")
    ap.add_argument("--system", default=DEFAULT_SYSTEM,
                    help="libretro-database system name")
    ap.add_argument("--json-out", default="", help="write the result here too")
    ap.add_argument("--refresh", action="store_true", help="ignore the cache")
    ap.add_argument("--no-description", action="store_true",
                    help="skip the Wikipedia summary (publisher/developer/year only)")
    args = ap.parse_args()
    if not re.fullmatch(r"[0-9A-Fa-f]{8}", args.crc32.strip()):
        print(f"error: --crc32 must be 8 hex digits, got {args.crc32!r}", file=sys.stderr)
        return 2
    hit = lookup(args.crc32, args.system, force=args.refresh,
                 description=not args.no_description)
    hit["source"] = LIBRETRO_RAW + "metadat/ (no-intro, publisher, developer, releaseyear DATs, by CRC32)"
    text = json.dumps(hit, indent=2)
    if args.json_out:
        Path(args.json_out).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if any(k in hit for k in ("name", "publisher", "developer", "year")) else 1


if __name__ == "__main__":
    raise SystemExit(main())
