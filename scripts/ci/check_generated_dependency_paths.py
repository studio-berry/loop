#!/usr/bin/env python3
"""Fail if generated dependency state is tracked in the source repository."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED_ROOTS = (
    ".docker-vcpkg",
    ".vcpkg-binary-cache/",
    "vcpkg-binary-cache/",
    "vcpkg_installed/",
    "vcpkg/downloads/",
    "vcpkg/packages/",
    "vcpkg/buildtrees/",
    "vcpkg/installed/",
    "vcpkg/archives/",
)


def is_generated_dependency_path(path: str) -> bool:
    """Return whether a repository-relative path is generated dependency state."""
    normalized = path.replace("\\", "/")
    return normalized == ".docker-vcpkg" or normalized.startswith(GENERATED_ROOTS)


def tracked_paths(root: Path = ROOT) -> list[str]:
    """Read tracked paths from Git without inspecting the working tree."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


def validate_repository(root: Path = ROOT) -> list[str]:
    return [path for path in tracked_paths(root) if is_generated_dependency_path(path)]


def main() -> int:
    try:
        violations = validate_repository()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: unable to inspect tracked paths: {exc}", file=sys.stderr)
        return 1
    if violations:
        print("ERROR: generated dependency paths must not be tracked:", file=sys.stderr)
        for path in violations:
            print(f"  {path}", file=sys.stderr)
        return 1
    print("Generated dependency path policy passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
