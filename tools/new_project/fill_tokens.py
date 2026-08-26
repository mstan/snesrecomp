#!/usr/bin/env python3
"""Substitute @TOKEN@ placeholders in a template.

Deliberately dumber than a template engine: templates are read by people who
are about to edit the file it produced, so a token has to be greppable in both
directions. Unknown tokens are an error rather than an empty string — a silent
blank in a CMakeLists or a CI workflow surfaces much later and much worse.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

TOKEN = re.compile(r"@([A-Z0-9_]+)@")


def render(text: str, values: dict[str, str]) -> tuple[str, list[str]]:
    missing: list[str] = []

    def replace(match: re.Match) -> str:
        key = match.group(1)
        if key not in values:
            missing.append(key)
            return match.group(0)
        return values[key]

    return TOKEN.sub(replace, text), missing


def fill(src: pathlib.Path, dst: pathlib.Path, values: dict[str, str],
         *, allow_missing: bool = False) -> None:
    rendered, missing = render(src.read_text(encoding="utf-8"), values)
    if missing and not allow_missing:
        unique = sorted(set(missing))
        raise SystemExit(
            f"fill_tokens: {src.name} has unset tokens: {', '.join(unique)}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(rendered, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("template")
    parser.add_argument("output")
    parser.add_argument("--set", action="append", default=[],
                        metavar="KEY=VALUE", help="token value (repeatable)")
    parser.add_argument("--allow-missing", action="store_true",
                        help="leave unknown tokens in place instead of failing")
    args = parser.parse_args()

    values: dict[str, str] = {}
    for item in args.set:
        if "=" not in item:
            parser.error(f"--set needs KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        values[key] = value

    fill(pathlib.Path(args.template), pathlib.Path(args.output), values,
         allow_missing=args.allow_missing)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
