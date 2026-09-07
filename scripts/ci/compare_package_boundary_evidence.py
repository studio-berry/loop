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
GRAPHICS_API = re.compile(r"graphics_api=([a-z0-9]+)")
NATIVE_GRAPHICS = frozenset({"d3d11", "d3d12", "opengl", "vulkan", "metal"})


class PairError(ValueError):
    """Raised when the two package evidence records cannot be paired."""


def load(path: Path, expected_platform: str) -> dict[str, Any]:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PairError(f"unable to read evidence {path}: {exc}") from exc
    if evidence.get("schema_version") != 1 or evidence.get("kind") != "loop-package-boundary-evidence":
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


def _read_transcript(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PairError(f"unable to read transcript {path}: {exc}") from exc


def _require_source_sha(path: Path, text: str, expected_sha: str) -> None:
    if expected_sha.lower() not in text.lower():
        raise PairError(f"transcript does not record expected source SHA: {path}")


def _require_graphics_api(path: Path, text: str, *, native: bool) -> str:
    match = GRAPHICS_API.search(text)
    if not match:
        raise PairError(f"transcript does not report graphics_api: {path}")
    api = match.group(1)
    if native:
        if api not in NATIVE_GRAPHICS:
            raise PairError(
                f"native accessibility evidence ran through software graphics "
                f"(graphics_api={api}): {path}"
            )
    elif api != "software":
        raise PairError(f"software accessibility evidence did not select software (graphics_api={api}): {path}")
    return api


def load_operator_transcripts(linux_dir: Path, windows_dir: Path, expected_sha: str) -> dict[str, str]:
    linux_native = linux_dir / "quick-a11y-native.txt"
    linux_software = linux_dir / "quick-a11y-software.txt"
    linux_smoke = linux_dir / "appimage-smoke.txt"
    windows_native = windows_dir / "quick-a11y-native.txt"
    windows_software = windows_dir / "quick-a11y-software.txt"
    windows_smoke = windows_dir / "msi-smoke.txt"
    required = (
        linux_native,
        linux_software,
        linux_smoke,
        windows_native,
        windows_software,
        windows_smoke,
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise PairError(f"operator evidence transcripts are missing: {missing}")

    linux_native_text = _read_transcript(linux_native)
    linux_software_text = _read_transcript(linux_software)
    linux_smoke_text = _read_transcript(linux_smoke)
    windows_native_text = _read_transcript(windows_native)
    windows_software_text = _read_transcript(windows_software)
    windows_smoke_text = _read_transcript(windows_smoke)

    for path, text in (
        (linux_native, linux_native_text),
        (linux_software, linux_software_text),
        (linux_smoke, linux_smoke_text),
        (windows_native, windows_native_text),
        (windows_software, windows_software_text),
        (windows_smoke, windows_smoke_text),
    ):
        _require_source_sha(path, text, expected_sha)

    linux_native_api = _require_graphics_api(linux_native, linux_native_text, native=True)
    windows_native_api = _require_graphics_api(windows_native, windows_native_text, native=True)
    _require_graphics_api(linux_software, linux_software_text, native=False)
    _require_graphics_api(windows_software, windows_software_text, native=False)

    if "OK: LoopEditor operator launch remained alive" not in linux_smoke_text:
        raise PairError(f"Linux AppImage smoke skipped operator Editor launch: {linux_smoke}")
    if "OK: LoopEditor launched without immediate crash" not in windows_smoke_text:
        raise PairError(f"Windows MSI smoke skipped Editor launch: {windows_smoke}")

    return {
        "linux_native_graphics_api": linux_native_api,
        "windows_native_graphics_api": windows_native_api,
    }


def compare(
    linux_path: Path,
    windows_path: Path,
    expected_sha: str,
    linux_dir: Path | None = None,
    windows_dir: Path | None = None,
) -> dict[str, Any]:
    if not FULL_SHA.fullmatch(expected_sha):
        raise PairError("expected source SHA must be a full 40-character Git SHA")
    if (linux_dir is None) != (windows_dir is None):
        raise PairError("linux and windows evidence directories must be supplied together")
    linux = load(linux_path, "linux")
    windows = load(windows_path, "windows")
    source_sha = str(linux.get("source_sha", "")).lower()
    if source_sha != expected_sha.lower() or str(windows.get("source_sha", "")).lower() != source_sha:
        raise PairError(
            "package evidence source SHA mismatch: "
            f"expected={expected_sha.lower()} linux={source_sha} windows={windows.get('source_sha')}"
        )
    pair: dict[str, Any] = {
        "schema_version": 1,
        "kind": "loop-package-boundary-pair",
        "source_sha": source_sha,
        "packages": {
            "linux": linux["package"],
            "windows": windows["package"],
        },
        "status": "passed",
    }
    if linux_dir is not None and windows_dir is not None:
        pair["operator_evidence"] = load_operator_transcripts(linux_dir, windows_dir, source_sha)
    return pair


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--linux-dir", type=Path, default=None)
    parser.add_argument("--windows-dir", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        pair = compare(args.linux, args.windows, args.source_sha, args.linux_dir, args.windows_dir)
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
