#!/usr/bin/env python3
"""Validate the Linux and Windows package-boundary evidence pair."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Sequence


FULL_SHA = re.compile(r"^[0-9a-fA-F]{40}$")


class PairError(ValueError):
    """Raised when the two package evidence records cannot be paired."""


def load(path: Path, expected_platform: str) -> dict[str, Any]:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PairError(f"unable to read evidence {path}: {exc}") from exc
    if evidence.get("schema_version") != 1 or evidence.get("kind") != "loupe-package-boundary-evidence":
        raise PairError(f"unsupported evidence schema: {path}")
    if evidence.get("platform") != expected_platform:
        raise PairError(f"expected {expected_platform} evidence, got {evidence.get('platform')}: {path}")
    if evidence.get("status") != "passed":
        raise PairError(f"package evidence is not passed: {path}")
    if evidence.get("forbidden_findings"):
        raise PairError(f"package evidence contains forbidden findings: {path}")
    checks = evidence.get("checks")
    required_checks = (
        "all_payload_files_hashed",
        "all_binary_files_inspected",
        "target_architecture_matches",
        "qt6widgets_absent",
        "qt6widgets_surface_absent",
        "unresolved_non_system_dependencies_absent",
    )
    if not isinstance(checks, dict) or any(checks.get(name) is not True for name in required_checks):
        raise PairError(f"package evidence checks are incomplete: {path}")
    if not FULL_SHA.fullmatch(str(evidence.get("source_sha", ""))):
        raise PairError(f"package evidence source SHA is not full length: {path}")
    package = evidence.get("package")
    expected_format = "AppImage" if expected_platform == "linux" else "MSI"
    if (
        not isinstance(package, dict)
        or package.get("format") != expected_format
        or not re.fullmatch(r"[0-9a-fA-F]{64}", str(package.get("sha256", "")))
    ):
        raise PairError(f"package identity is incomplete: {path}")
    return evidence


def compare(linux_path: Path, windows_path: Path, expected_sha: str) -> dict[str, Any]:
    if not FULL_SHA.fullmatch(expected_sha):
        raise PairError("expected source SHA must be a full 40-character Git SHA")
    linux = load(linux_path, "linux")
    windows = load(windows_path, "windows")
    source_sha = str(linux.get("source_sha", "")).lower()
    if source_sha != expected_sha.lower() or str(windows.get("source_sha", "")).lower() != source_sha:
        raise PairError(
            "package evidence source SHA mismatch: "
            f"expected={expected_sha.lower()} linux={source_sha} windows={windows.get('source_sha')}"
        )
    return {
        "schema_version": 1,
        "kind": "loupe-package-boundary-pair",
        "source_sha": source_sha,
        "packages": {
            "linux": linux["package"],
            "windows": windows["package"],
        },
        "status": "passed",
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        pair = compare(args.linux, args.windows, args.source_sha)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(pair, indent=2) + "\n", encoding="utf-8")
    except PairError as exc:
        print(f"Package boundary pair FAILED: {exc}", file=sys.stderr)
        return 1
    print(f"Package boundary pair passed: source_sha={pair['source_sha']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
