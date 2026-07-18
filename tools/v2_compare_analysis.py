"""Compare whole-program manifests at the emission compatibility boundary."""

from __future__ import annotations

import argparse
import json
import pathlib


def _load(path: pathlib.Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc


def _semantic_node(node: dict) -> dict:
    return {key: value for key, value in node.items() if key != "digest"}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="compare Python/native LLE-first analysis manifests")
    parser.add_argument("--expected", required=True)
    parser.add_argument("--actual", required=True)
    parser.add_argument(
        "--strict-summaries", action="store_true",
        help="also require diagnostic instruction ranges, reasons, and edges")
    args = parser.parse_args()

    expected_path = pathlib.Path(args.expected).resolve()
    actual_path = pathlib.Path(args.actual).resolve()
    try:
        expected = _load(expected_path)
        actual = _load(actual_path)
    except ValueError as exc:
        parser.error(str(exc))

    differences = []
    for field in ("format_version", "roots", "exit_modes", "exit_mode_sets"):
        if expected.get(field) != actual.get(field):
            differences.append(field)

    expected_nodes = expected.get("nodes", {})
    actual_nodes = actual.get("nodes", {})
    if set(expected_nodes) != set(actual_nodes):
        missing = sorted(set(expected_nodes) - set(actual_nodes))
        extra = sorted(set(actual_nodes) - set(expected_nodes))
        differences.append(
            f"node keys (missing={missing[:5]}, extra={extra[:5]})")
    for key in sorted(set(expected_nodes) & set(actual_nodes)):
        if (expected_nodes[key].get("disposition")
                != actual_nodes[key].get("disposition")):
            differences.append(f"{key}: disposition")
        if (args.strict_summaries
                and _semantic_node(expected_nodes[key])
                != _semantic_node(actual_nodes[key])):
            differences.append(f"{key}: summary")
        if len(differences) >= 25:
            break

    if differences:
        for difference in differences:
            print(f"DRIFT: {difference}")
        print(f"v2_compare_analysis: {len(differences)} difference(s)")
        return 1

    print(
        "v2_compare_analysis: emission contract matches "
        f"({len(expected_nodes)} variants)")
    if not args.strict_summaries:
        print(
            "v2_compare_analysis: diagnostic graph summaries were not "
            "required; pass --strict-summaries to audit them")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
