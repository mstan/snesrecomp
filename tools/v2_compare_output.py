"""Byte-compare two complete v2 generated-output directories."""

from __future__ import annotations

import argparse
import filecmp
import pathlib


def _files(root: pathlib.Path) -> dict[str, pathlib.Path]:
    if not root.is_dir():
        raise ValueError(f"generated-output directory does not exist: {root}")
    return {
        path.relative_to(root).as_posix(): path
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def compare_output(expected: pathlib.Path, actual: pathlib.Path) -> list[str]:
    expected_files = _files(expected)
    actual_files = _files(actual)
    problems = []
    for relative in sorted(expected_files.keys() - actual_files.keys()):
        problems.append(f"MISSING: {relative}")
    for relative in sorted(actual_files.keys() - expected_files.keys()):
        problems.append(f"UNEXPECTED: {relative}")
    for relative in sorted(expected_files.keys() & actual_files.keys()):
        if not filecmp.cmp(
                expected_files[relative], actual_files[relative],
                shallow=False):
            problems.append(f"DRIFT: {relative}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description="verify two v2 output trees are byte-identical")
    parser.add_argument("--expected", required=True)
    parser.add_argument("--actual", required=True)
    args = parser.parse_args()

    expected = pathlib.Path(args.expected).resolve()
    actual = pathlib.Path(args.actual).resolve()
    try:
        problems = compare_output(expected, actual)
        file_count = len(_files(expected))
    except ValueError as exc:
        parser.error(str(exc))
    if problems:
        for problem in problems:
            print(problem)
        print(f"v2_compare_output: {len(problems)} difference(s)")
        return 1
    print(f"v2_compare_output: {file_count} files byte-identical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
