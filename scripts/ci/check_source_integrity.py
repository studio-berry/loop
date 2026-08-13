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

import json
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

EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
# Legacy hashed seeds only. Harness corpora under Fuzz/corpus/<harness>/ are
# owned by scripts/ci/check_fuzz_corpus.py and Fuzz/corpus/manifest.json.
FUZZ_CORPUS_PREFIX = "Fuzz/corpus/regression/"
FUZZ_MANIFEST_PATH = "Fuzz/corpus/regression/manifest.json"
FUZZ_MANIFEST_EXEMPT = frozenset({".gitkeep", "LICENSE", "README.md", "manifest.json"})
PREFLIGHT_FIXTURES_PREFIX = "loupe-preflight/testdata/fixtures/"
PREFLIGHT_MANIFEST_PATH = "loupe-preflight/testdata/fixtures/manifest.json"
WHITESPACE_CHECK = re.compile(r"^([^:]+):(\d+):\s+(.+)$")


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


def whitespace_violations(root: Path = ROOT) -> list[tuple[str, str]]:
    """Return whitespace problems reported by `git diff --check` on tracked text."""
    result = subprocess.run(
        ["git", "-C", str(root), "diff", "--check", EMPTY_TREE, "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    )
    violations: list[tuple[str, str]] = []
    for line in (result.stdout + result.stderr).splitlines():
        match = WHITESPACE_CHECK.match(line)
        if not match:
            continue
        path, message = match.group(1), match.group(3).strip().rstrip(".")
        violations.append((path, f"whitespace: {message}"))
    return violations


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def fuzz_corpus_violations(root: Path = ROOT) -> list[tuple[str, str]]:
    """Return tracked fuzz seeds that are missing from the regression manifest."""
    tracked_seeds = [
        path
        for path in tracked_paths(root)
        if normalize(path).startswith(FUZZ_CORPUS_PREFIX)
        and normalize(path).rsplit("/", 1)[-1] not in FUZZ_MANIFEST_EXEMPT
    ]
    if not tracked_seeds:
        return []

    manifest_path = root / FUZZ_MANIFEST_PATH
    if not manifest_path.is_file():
        return [(FUZZ_MANIFEST_PATH, "fuzz corpus manifest is missing")]

    manifest = _load_json(manifest_path)
    if not isinstance(manifest, dict):
        return [(FUZZ_MANIFEST_PATH, "fuzz corpus manifest must be a JSON object")]

    entries = manifest.get("entries")
    if not isinstance(entries, list):
        return [(FUZZ_MANIFEST_PATH, "fuzz corpus manifest must contain an entries array")]

    allowlisted: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            return [(FUZZ_MANIFEST_PATH, f"entries[{index}] must be an object")]
        file_name = entry.get("file")
        rationale = entry.get("rationale")
        if not isinstance(file_name, str) or not file_name:
            return [(FUZZ_MANIFEST_PATH, f"entries[{index}] is missing a file name")]
        if not isinstance(rationale, str) or not rationale.strip():
            return [(FUZZ_MANIFEST_PATH, f"entries[{index}] is missing a rationale")]
        allowlisted.add(file_name)

    violations: list[tuple[str, str]] = []
    for path in tracked_seeds:
        normalized = normalize(path)
        basename = normalized.rsplit("/", 1)[-1]
        if basename not in allowlisted:
            violations.append((normalized, "unmanifested fuzz corpus seed"))
    return violations


def preflight_pdf_violations(root: Path = ROOT) -> list[tuple[str, str]]:
    """Return tracked preflight fixture PDFs that are not listed in manifest.json."""
    tracked_pdfs = [
        path
        for path in tracked_paths(root)
        if normalize(path).startswith(PREFLIGHT_FIXTURES_PREFIX)
        and normalize(path).endswith(".pdf")
    ]
    if not tracked_pdfs:
        return []

    manifest_path = root / PREFLIGHT_MANIFEST_PATH
    if not manifest_path.is_file():
        return [(PREFLIGHT_MANIFEST_PATH, "preflight fixture manifest is missing")]

    manifest = _load_json(manifest_path)
    if not isinstance(manifest, list):
        return [(PREFLIGHT_MANIFEST_PATH, "preflight fixture manifest must be a JSON array")]

    allowlisted: set[str] = set()
    for index, entry in enumerate(manifest):
        if not isinstance(entry, dict):
            return [(PREFLIGHT_MANIFEST_PATH, f"entry[{index}] must be an object")]
        pdf_name = entry.get("pdf")
        if isinstance(pdf_name, str) and pdf_name:
            allowlisted.add(pdf_name)

    violations: list[tuple[str, str]] = []
    for path in tracked_pdfs:
        normalized = normalize(path)
        basename = normalized.rsplit("/", 1)[-1]
        if basename not in allowlisted:
            violations.append((normalized, "unmanifested preflight fixture PDF"))
    return violations


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

    violations.extend(whitespace_violations(root))
    violations.extend(fuzz_corpus_violations(root))
    violations.extend(preflight_pdf_violations(root))
    return violations


def main() -> int:
    try:
        violations = validate_repository()
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
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
