#!/usr/bin/env python3
"""Validate the tracked fuzz regression corpus against manifest.json."""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CORPUS_ROOT = ROOT / "Fuzz" / "corpus"
MANIFEST_PATH = CORPUS_ROOT / "manifest.json"

HARNESS_TARGETS = frozenset(
    {
        "fuzz_pdf_parser",
        "fuzz_stream_filters",
        "fuzz_content_stream",
        "fuzz_images",
    }
)

REQUIRED_CASE_FIELDS = frozenset(
    {
        "id",
        "path",
        "harness",
        "origin",
        "issue",
        "sha256",
        "expected",
        "minimized",
    }
)

ALLOWED_ORIGINS = frozenset({"fuzz-finding", "synthetic"})
ALLOWED_EXPECTED = frozenset({"terminates-without-crash"})
IGNORED_BASENAMES = frozenset({".gitkeep", "LICENSE", "README.md", "manifest.json"})
KEBAB_CASE = re.compile(r"^[a-z][a-z0-9-]*$")
HASH_NAME = re.compile(r"^[0-9a-f]{40}$")


def normalize(path: str) -> str:
    """Repository-relative path with forward slashes."""
    return path.replace("\\", "/")


def sha256_file(path: Path) -> str:
    """Return the lowercase hex SHA-256 digest of a file."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(root: Path = ROOT) -> dict:
    """Load and return the corpus manifest."""
    manifest_path = root / MANIFEST_PATH.relative_to(ROOT)
    with manifest_path.open(encoding="utf-8") as handle:
        return json.load(handle)


def validate_manifest(manifest: dict, root: Path = ROOT) -> list[tuple[str, str]]:
    """Return (subject, reason) for every manifest or corpus violation."""
    violations: list[tuple[str, str]] = []

    if manifest.get("schema_version") != 1:
        violations.append(("manifest.json", "schema_version must be 1"))
        return violations

    cases = manifest.get("cases")
    if not isinstance(cases, list):
        violations.append(("manifest.json", "cases must be an array"))
        return violations

    seen_ids: set[str] = set()
    manifest_paths: set[str] = set()

    for index, case in enumerate(cases):
        label = f"cases[{index}]"
        if not isinstance(case, dict):
            violations.append((label, "case entry must be an object"))
            continue

        missing = REQUIRED_CASE_FIELDS - case.keys()
        if missing:
            violations.append((label, f"missing required fields: {sorted(missing)}"))
            continue

        case_id = case["id"]
        if not isinstance(case_id, str) or not KEBAB_CASE.match(case_id):
            violations.append((label, f"id must be kebab-case, got {case_id!r}"))
        elif case_id in seen_ids:
            violations.append((label, f"duplicate id {case_id!r}"))
        else:
            seen_ids.add(case_id)

        harness = case["harness"]
        if harness not in HARNESS_TARGETS:
            violations.append((label, f"unknown harness {harness!r}"))

        origin = case["origin"]
        if origin not in ALLOWED_ORIGINS:
            violations.append((label, f"unknown origin {origin!r}"))

        expected = case["expected"]
        if expected not in ALLOWED_EXPECTED:
            violations.append((label, f"unknown expected outcome {expected!r}"))

        if not isinstance(case["issue"], int):
            violations.append((label, "issue must be an integer"))
        if not isinstance(case["minimized"], bool):
            violations.append((label, "minimized must be a boolean"))

        rel_path = normalize(case["path"])
        manifest_paths.add(rel_path)

        if not rel_path.startswith("Fuzz/corpus/"):
            violations.append((rel_path, "path must live under Fuzz/corpus/"))
            continue

        parts = rel_path.split("/")
        if len(parts) < 4:
            violations.append((rel_path, "path must include a harness directory"))
            continue

        harness_dir = parts[2]
        if harness_dir not in HARNESS_TARGETS:
            violations.append((rel_path, f"unknown harness directory {harness_dir!r}"))
        elif harness_dir != harness:
            violations.append(
                (rel_path, f"path harness directory {harness_dir!r} != harness {harness!r}")
            )

        basename = parts[-1]
        if HASH_NAME.match(basename):
            violations.append((rel_path, "hash-named seed files are not allowed"))

        absolute = root / rel_path
        if not absolute.is_file():
            violations.append((rel_path, "manifest path does not exist"))
            continue

        digest = case["sha256"]
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            violations.append((rel_path, "sha256 must be a 64-character lowercase hex digest"))
            continue

        actual = sha256_file(absolute)
        if actual != digest:
            violations.append(
                (rel_path, f"sha256 mismatch (manifest {digest}, actual {actual})")
            )

    for harness in sorted(HARNESS_TARGETS):
        harness_dir = root / "Fuzz" / "corpus" / harness
        if not harness_dir.is_dir():
            violations.append((f"Fuzz/corpus/{harness}", "harness directory is missing"))
            continue

        for path in sorted(harness_dir.iterdir()):
            if not path.is_file():
                continue

            basename = path.name
            if basename in IGNORED_BASENAMES:
                continue

            rel_path = normalize(str(path.relative_to(root)))
            if HASH_NAME.match(basename):
                violations.append((rel_path, "hash-named seed files are not allowed"))
                continue

            if rel_path not in manifest_paths:
                violations.append((rel_path, "tracked seed is missing from manifest.json"))

    return violations


def validate_corpus(root: Path = ROOT) -> list[tuple[str, str]]:
    """Return (subject, reason) for every corpus violation."""
    try:
        manifest = load_manifest(root)
    except (OSError, json.JSONDecodeError) as exc:
        return [("manifest.json", f"unable to load manifest: {exc}")]

    return validate_manifest(manifest, root)


def main() -> int:
    violations = validate_corpus()
    if violations:
        print("ERROR: fuzz corpus failed validation:", file=sys.stderr)
        for subject, reason in violations:
            print(f"  {subject}: {reason}", file=sys.stderr)
        return 1
    print("Fuzz corpus policy passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
