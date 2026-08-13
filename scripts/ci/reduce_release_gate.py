#!/usr/bin/env python3
"""Reduce GitHub Actions ``needs`` results into a single release-gate verdict.

The ``release_ok`` job always runs and must fail when any required dependency
failed, was cancelled, was skipped, or did not report. Success is only valid
when every required job result is ``success``.
"""

from __future__ import annotations

import json
import os
import sys
from typing import Any


REQUIRED_JOBS = (
    "source_integrity",
    "linux",
    "windows",
    "documentation",
    "fuzz_regression",
    "package_contract",
    "supply_chain",
)


def reduce_needs(needs: Any) -> list[str]:
    """Return human-readable violations for a GitHub Actions needs object."""
    if needs is None:
        return ["needs context is missing"]
    if not isinstance(needs, dict):
        return [f"needs context must be an object, got {type(needs).__name__}"]

    violations: list[str] = []
    for name in REQUIRED_JOBS:
        entry = needs.get(name)
        if entry is None:
            violations.append(f"{name}: missing (did not report)")
            continue
        if not isinstance(entry, dict):
            violations.append(f"{name}: missing result")
            continue
        result = entry.get("result")
        if result is None or result == "":
            violations.append(f"{name}: missing result")
            continue
        if result != "success":
            violations.append(f"{name}: {result}")
    return violations


def main() -> int:
    raw = os.environ.get("RELEASE_GATE_NEEDS")
    if raw is None:
        raw = sys.stdin.read()
    try:
        needs = json.loads(raw) if str(raw).strip() else {}
    except json.JSONDecodeError as exc:
        print(f"ERROR: invalid needs JSON: {exc}", file=sys.stderr)
        return 1

    violations = reduce_needs(needs)
    if violations:
        for violation in violations:
            print(f"ERROR: {violation}", file=sys.stderr)
        return 1
    print("Release gate passed: all required jobs succeeded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
