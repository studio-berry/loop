#!/usr/bin/env python3
"""Fail if the tracked source tree carries build output or unresolved conflicts.

Complements check_generated_dependency_paths.py, which covers vcpkg state. This
check exists because PR #188 shipped a 207-file Docker build tree (including a
46 MB shared library), a root debug log, and a committed merge-conflict block
that broke the Pdf4QtLibCore build.

Everything here is judged from `git ls-files`, so it reports what is *tracked*
and not merely present in a dirty working tree.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

# Directories that only ever hold build output. The optional -suffix covers
# sibling trees such as build-fuzz-docker/ without one entry per variant. This
# matches a directory component only, so a file named build-notes.md is fine.
BUILD_TREE = re.compile(r"^build(?:-[^/]*)?/")

# Files that are configure/build state wherever they appear.
FORBIDDEN_BASENAMES = frozenset({"CMakeCache.txt", "CMakeCacheDefault.txt"})

# One-off investigation scratch. Shared checks belong in scripts/ci/.
ROOT_DEBUG_LOG = re.compile(r"^debug-[^/]*\.log$")
DEBUG_SCRIPT = re.compile(r"^scripts/debug-[^/]*\.(?:sh|ps1|py|cmd|bat)$")

# Only the two markers that cannot appear in ordinary prose are matched. A bare
# "=======" is a common Markdown heading rule and is deliberately not a trigger.
CONFLICT_MARKER = re.compile(r"^(?:<{7}|>{7})(?: |$)")

MAX_TRACKED_BYTES = 5 * 1024 * 1024

# Tracked files permitted to exceed MAX_TRACKED_BYTES, each with the reason it
# earns the exception. Keep this empty unless a fixture genuinely needs the size.
LARGE_FILE_ALLOWLIST: dict[str, str] = {}


def normalize(path: str) -> str:
    """Repository-relative path with forward slashes."""
    return path.replace("\\", "/")


def forbidden_path_reason(path: str) -> str | None:
    """Reason this path must not be tracked, or None if it is acceptable."""
    normalized = normalize(path)
    if normalized == ".docker-vcpkg" or normalized.startswith(".docker-vcpkg/"):
        return "generated dependency state"
    if BUILD_TREE.match(normalized):
        return "build output directory"
    if normalized.rsplit("/", 1)[-1] in FORBIDDEN_BASENAMES:
        return "build configuration cache"
    if ROOT_DEBUG_LOG.match(normalized):
        return "one-off debug log at the repository root"
    if DEBUG_SCRIPT.match(normalized):
        return "one-off debug script (shared checks belong in scripts/ci/)"
    return None


def has_conflict_markers(text: str) -> bool:
    """Whether the text contains an unresolved merge-conflict marker."""
    return any(CONFLICT_MARKER.match(line) for line in text.splitlines())


def oversized_reason(path: str, size: int) -> str | None:
    """Reason this tracked file is too large, or None if it is acceptable."""
    if size <= MAX_TRACKED_BYTES:
        return None
    if normalize(path) in LARGE_FILE_ALLOWLIST:
        return None
    return f"tracked file is {size / 1024 / 1024:.1f} MB, over the {MAX_TRACKED_BYTES // 1024 // 1024} MB cap"


def tracked_paths(root: Path = ROOT) -> list[str]:
    """Read tracked paths from Git without inspecting the working tree."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


def validate_repository(root: Path = ROOT) -> list[tuple[str, str]]:
    """Return (path, reason) for every tracked file that violates a rule."""
    violations: list[tuple[str, str]] = []

    for path in tracked_paths(root):
        reason = forbidden_path_reason(path)
        if reason:
            violations.append((path, reason))
            continue

        absolute = root / path
        try:
            if not absolute.is_file():
                # Submodule entries and symlinks to nowhere have no content here.
                continue
            size = absolute.stat().st_size
        except OSError:
            continue

        reason = oversized_reason(path, size)
        if reason:
            violations.append((path, reason))
            continue

        try:
            text = absolute.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            # Binary or unreadable: nothing to scan for conflict markers.
            continue

        if has_conflict_markers(text):
            violations.append((path, "unresolved merge-conflict marker"))

    return violations


def main() -> int:
    try:
        violations = validate_repository()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: unable to inspect tracked paths: {exc}", file=sys.stderr)
        return 1
    if violations:
        print("ERROR: tracked source tree failed the integrity check:", file=sys.stderr)
        for path, reason in violations:
            print(f"  {path}: {reason}", file=sys.stderr)
        return 1
    print("Source integrity policy passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
