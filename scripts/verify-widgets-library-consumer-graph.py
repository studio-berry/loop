#!/usr/bin/env python3
"""Fail-closed contract for Session 05 Issue 16 Widgets library consumer graph."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GRAPH_PATH = ROOT / "docs/generated/widgets-library-consumer-graph.json"
LIBRARIES = ("LoupeLibWidgets", "LoupeLibGui")
ALLOWED_CONSUMER_CLASSES = frozenset(
    {
        "installed-product-plugin",
        "installed-product-application",
        "build-only-plugin",
        "build-only-retired-executable",
        "profile-disabled-executable",
        "widgets-library",
        "developer-tool-consumer",
        "test-consumer",
    }
)
ALLOWED_BINDINGS = frozenset({"widgets-bound", "neutral-relocatable"})
ALLOWED_OWNERS = frozenset({"LoupeLibCore", "delete-with-widgets-library"})


class ContractError(ValueError):
    pass


def _run_generator() -> None:
    completed = subprocess.run(
        [sys.executable, str(ROOT / "scripts/generate_widgets_library_consumer_graph.py"), "--check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise ContractError(f"generated graph is stale or invalid: {detail}")


def validate_graph(root: Path) -> None:
    _run_generator()
    try:
        graph = json.loads(GRAPH_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read {GRAPH_PATH.name}: {exc}") from exc

    if graph.get("schema_version") != 1 or graph.get("evidence_kind") != "widgets-library-consumer-graph":
        raise ContractError("unsupported consumer graph schema")
    if graph.get("issue") != 16:
        raise ContractError("consumer graph must be tied to Issue 16")

    inventory = json.loads((root / "docs/generated/phase5-widgets-inventory.json").read_text(encoding="utf-8"))
    target_map = {row["id"]: row for row in inventory["targets"]}
    for library in LIBRARIES:
        if library not in target_map:
            raise ContractError(f"inventory missing library target: {library}")

    graph_consumers = graph.get("consumers")
    if not isinstance(graph_consumers, list):
        raise ContractError("consumers must be an array")

    observed: dict[str, set[str]] = {library: set() for library in LIBRARIES}
    for row in graph_consumers:
        library = row.get("library")
        consumer = row.get("consumer")
        if library not in LIBRARIES:
            raise ContractError(f"unknown library in consumer row: {library!r}")
        if consumer not in target_map:
            raise ContractError(f"consumer row references unknown target: {consumer!r}")
        if consumer not in target_map[library].get("consumers", []):
            raise ContractError(f"consumer row not present in inventory reverse graph: {library}->{consumer}")
        consumer_class = row.get("consumer_class")
        if consumer_class not in ALLOWED_CONSUMER_CLASSES:
            raise ContractError(f"unclassified consumer class for {consumer}: {consumer_class!r}")
        if row.get("replacement_owner") in {None, "", "unassigned"}:
            raise ContractError(f"missing replacement owner for {library}->{consumer}")
        observed[library].add(consumer)

    for library in LIBRARIES:
        expected = set(target_map[library].get("consumers", []))
        if observed[library] != expected:
            missing = sorted(expected - observed[library])
            extra = sorted(observed[library] - expected)
            raise ContractError(
                f"{library} consumer graph drift missing={missing} extra={extra}"
            )

    neutral_code = graph.get("neutral_code")
    if not isinstance(neutral_code, list) or not neutral_code:
        raise ContractError("neutral_code must be a non-empty array")

    for row in neutral_code:
        if row.get("binding") not in ALLOWED_BINDINGS:
            raise ContractError(f"invalid neutral-code binding for {row.get('path')}")
        if row.get("correct_owner") not in ALLOWED_OWNERS:
            raise ContractError(f"invalid neutral-code owner for {row.get('path')}")

    acceptance = graph.get("acceptance")
    if not isinstance(acceptance, dict):
        raise ContractError("acceptance section is required")
    if acceptance.get("unknown_product_consumers"):
        raise ContractError(
            "unknown product consumers remain: "
            + ", ".join(acceptance["unknown_product_consumers"])
        )
    if acceptance.get("unclassified_neutral_code"):
        raise ContractError(
            "unclassified neutral code remains: "
            + ", ".join(acceptance["unclassified_neutral_code"])
        )
    blockers = acceptance.get("installed_product_blockers")
    if not isinstance(blockers, list) or not blockers:
        raise ContractError("expected installed product blockers to be recorded for Issue 16")
    if acceptance.get("deletion_safe"):
        raise ContractError("Issue 16 graph must record deletion_safe=false while blockers remain")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        validate_graph(args.root.resolve())
    except ContractError as exc:
        print(f"Widgets library consumer graph FAILED: {exc}", file=sys.stderr)
        return 1
    print("Widgets library consumer graph ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
