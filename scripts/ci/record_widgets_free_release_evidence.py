#!/usr/bin/env python3
"""Record exact-SHA evidence for a Widgets-free release qualification run."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


REQUIRED_CACHE_MARKERS = (
    "LOOP_LOOP_DISTRIBUTION:BOOL=ON",
    "LOOP_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=OFF",
)
FORBIDDEN_CACHE_MARKERS = (
    "LOOP_BUILD_CODE_GENERATOR:BOOL=ON",
    "LOOP_BUILD_JBIG2_VIEWER:BOOL=ON",
    "LOOP_BUILD_EXAMPLE_GENERATOR:BOOL=ON",
    "LOOP_BUILD_CANVAS_BENCHMARK:BOOL=ON",
)
REQUIRED_FORBIDDEN_QT_PREFIXES = {
    "qt6widgets",
    "qt6quickwidgets",
    "qtwidgets",
    "qtquickwidgets",
    "libqt6widgets",
    "libqt6quickwidgets",
    "qt6printsupport",
    "qtprintsupport",
    "libqt6printsupport",
    "libqtprintsupport",
}


class EvidenceError(ValueError):
    """Raised when the qualification inputs are not proven."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cache_value(text: str, key: str) -> str | None:
    prefix = f"{key}:"
    for line in text.splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1] if "=" in line else ""
    return None


def git_head(root: Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise EvidenceError(f"unable to resolve tested Git SHA: {completed.stderr.strip()}")
    return completed.stdout.strip()


def record_evidence(
    root: Path,
    qt_prefix: Path,
    manifest: Path,
    build_dir: Path,
    platform: str,
    qt_version: str,
    build_status: str,
    test_status: str,
    negative_configure_status: str,
) -> dict[str, object]:
    root = root.resolve()
    qt_prefix = qt_prefix.resolve()
    manifest = manifest.resolve()
    build_dir = build_dir.resolve()
    cache = build_dir / "CMakeCache.txt"

    if not manifest.is_file():
        raise EvidenceError(f"Qt prefix manifest does not exist: {manifest}")
    if not cache.is_file():
        raise EvidenceError(f"release configure cache does not exist: {cache}")

    manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(manifest_data, dict) or manifest_data.get("kind") != "loop-widgets-free-qt-prefix":
        raise EvidenceError("manifest is not a Widgets-free Qt prefix manifest")
    if not manifest_data.get("excluded_paths"):
        raise EvidenceError("manifest does not record excluded Widgets paths")
    if Path(str(manifest_data.get("destination_prefix"))).resolve() != qt_prefix:
        raise EvidenceError("manifest destination does not match the tested Qt prefix")
    manifest_prefixes = {
        str(prefix).casefold() for prefix in manifest_data.get("forbidden_name_prefixes", [])
    }
    missing_prefixes = sorted(REQUIRED_FORBIDDEN_QT_PREFIXES - manifest_prefixes)
    if missing_prefixes:
        raise EvidenceError(
            "manifest does not exclude the complete Widgets-bound Qt surface: "
            f"{missing_prefixes}"
        )
    required_configs = {str(config).casefold() for config in manifest_data.get("required_qt_configs", [])}
    if any("printsupport" in config for config in required_configs):
        raise EvidenceError("manifest incorrectly treats Qt6PrintSupport as required in the Widgets-free prefix")

    cache_text = cache.read_text(encoding="utf-8", errors="replace")
    missing = [marker for marker in REQUIRED_CACHE_MARKERS if marker not in cache_text]
    if missing:
        raise EvidenceError(f"release configure cache is missing required markers: {missing}")
    present_forbidden = [marker for marker in FORBIDDEN_CACHE_MARKERS if marker in cache_text]
    if present_forbidden:
        raise EvidenceError(f"release configure cache enables Widgets-bound targets: {present_forbidden}")

    expected_qt_dir = (qt_prefix / "lib" / "cmake" / "Qt6").resolve()
    actual_qt_dir = cache_value(cache_text, "Qt6_DIR")
    if not actual_qt_dir or Path(actual_qt_dir).resolve() != expected_qt_dir:
        raise EvidenceError(
            "release configure cache does not use the filtered Qt prefix: "
            f"expected {expected_qt_dir}, got {actual_qt_dir}"
        )

    source_sha = git_head(root)
    github_sha = os.environ.get("GITHUB_SHA", "").strip()
    if github_sha and github_sha != source_sha:
        raise EvidenceError(f"GITHUB_SHA {github_sha} does not match checked-out HEAD {source_sha}")

    return {
        "schema_version": 1,
        "kind": "loop-widgets-free-release-evidence",
        "status": "passed",
        "source_sha": source_sha,
        "github_sha": github_sha or None,
        "platform": platform,
        "qt_version": qt_version,
        "qt_prefix": str(qt_prefix),
        "qt_prefix_manifest_sha256": sha256_file(manifest),
        "configure": {
            "status": "passed",
            "cache": str(cache),
            "cache_sha256": sha256_file(cache),
            "qt6_dir": str(expected_qt_dir),
        },
        "build": {"status": build_status},
        "tests": {"status": test_status},
        "negative_widgets_configure": {"status": negative_configure_status},
        "release_options": {
            "LOOP_LOOP_DISTRIBUTION": True,
            "LOOP_BUILD_CANVAS_BENCHMARK": False,
            "LOOP_BUILD_CODE_GENERATOR": False,
            "LOOP_BUILD_JBIG2_VIEWER": False,
            "LOOP_BUILD_EXAMPLE_GENERATOR": False,
        },
        "excluded_widgets_paths": manifest_data["excluded_paths"],
        "excluded_widgets_bound_qt_paths": manifest_data["excluded_paths"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--qt-prefix", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--build-status", choices=("passed",), required=True)
    parser.add_argument("--test-status", choices=("passed",), required=True)
    parser.add_argument("--negative-configure-status", choices=("passed",), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        evidence = record_evidence(
            args.root,
            args.qt_prefix,
            args.manifest,
            args.build_dir,
            args.platform,
            args.qt_version,
            args.build_status,
            args.test_status,
            args.negative_configure_status,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    except (EvidenceError, OSError, json.JSONDecodeError) as exc:
        print(f"Widgets-free release evidence FAILED: {exc}", file=sys.stderr)
        return 1

    print(f"Widgets-free release evidence recorded for {evidence['source_sha']}: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
