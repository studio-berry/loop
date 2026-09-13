#!/usr/bin/env python3
"""Validate the lifecycle qualification corpus against manifest.json and schema."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS_DIR = ROOT / "UnitTests" / "testdata" / "lifecycle"
MANIFEST_PATH = CORPUS_DIR / "manifest.json"

ALLOWED_KINDS = frozenset(
    {
        "open",
        "render-preflight",
        "cancel",
        "replace-revision",
        "save-reopen",
        "rollback",
        "close",
    }
)
EXPECTED_INVARIANTS = frozenset(
    {
        "source-immutable",
        "cancel-is-terminal",
        "stale-results-rejected",
        "history-append-only",
    }
)
REPLAY_PROFILES = frozenset(
    {
        "inject-stale-acceptance",
        "inject-source-overwrite",
        "inject-history-mutation",
    }
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        payload = handle.read()
    digest.update(payload.replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def validate_trace(path: Path, *, max_commands: int) -> list[tuple[str, str]]:
    violations: list[tuple[str, str]] = []
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if payload.get("schema_kind") != "loop-lifecycle-trace":
        violations.append((path.name, "schema_kind must be loop-lifecycle-trace"))
    if payload.get("schema_version") != 1:
        violations.append((path.name, "schema_version must be 1"))
    if not payload.get("initial_artifact_digest"):
        violations.append((path.name, "initial_artifact_digest is required"))
    if not payload.get("observed_result"):
        violations.append((path.name, "observed_result is required"))
    if not isinstance(payload.get("shrink_history"), list):
        violations.append((path.name, "shrink_history must be an array"))
    commands = payload.get("commands")
    if not isinstance(commands, list) or not commands or len(commands) > max_commands:
        violations.append((path.name, f"commands must contain 1..{max_commands} entries"))
        return violations
    for index, command in enumerate(commands):
        if command.get("index") != index:
            violations.append((path.name, f"command index mismatch at {index}"))
        if command.get("kind") not in ALLOWED_KINDS:
            violations.append((path.name, f"unknown command kind at {index}"))
    expected = payload.get("expected_invariants")
    if not isinstance(expected, list):
        violations.append((path.name, "expected_invariants must be an array"))
    else:
        for value in expected:
            if value not in EXPECTED_INVARIANTS:
                violations.append((path.name, f"unexpected invariant {value!r}"))
    return violations


def validate_manifest() -> list[tuple[str, str]]:
    violations: list[tuple[str, str]] = []
    with MANIFEST_PATH.open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    if manifest.get("schema_kind") != "loop-lifecycle-corpus":
        violations.append(("manifest.json", "schema_kind must be loop-lifecycle-corpus"))
    max_commands = manifest.get("max_commands")
    if max_commands != 64:
        violations.append(("manifest.json", "max_commands must be 64"))

    passing = manifest.get("passing_traces")
    if not isinstance(passing, list) or not passing:
        violations.append(("manifest.json", "passing_traces must be a non-empty array"))
    else:
        for entry in passing:
            trace_file = entry.get("trace_file")
            if not trace_file:
                violations.append(("manifest.json", "passing trace missing trace_file"))
                continue
            path = CORPUS_DIR / trace_file
            if not path.is_file():
                violations.append((trace_file, "missing corpus file"))
                continue
            if entry.get("sha256") != sha256_file(path):
                violations.append((trace_file, "sha256 mismatch"))
            violations.extend(validate_trace(path, max_commands=max_commands))

    failures = manifest.get("failure_traces")
    if not isinstance(failures, list) or not failures:
        violations.append(("manifest.json", "failure_traces must be a non-empty array"))
    else:
        for entry in failures:
            trace_file = entry.get("trace_file")
            profile = entry.get("replay_profile")
            if profile not in REPLAY_PROFILES:
                violations.append((trace_file or "manifest.json", f"invalid replay_profile {profile!r}"))
            if not entry.get("expected_violation"):
                violations.append((trace_file or "manifest.json", "expected_violation is required"))
            if not trace_file:
                continue
            path = CORPUS_DIR / trace_file
            if not path.is_file():
                violations.append((trace_file, "missing failure trace"))
                continue
            if entry.get("sha256") != sha256_file(path):
                violations.append((trace_file, "sha256 mismatch"))
            violations.extend(validate_trace(path, max_commands=max_commands))
            payload = json.loads(path.read_text(encoding="utf-8"))
            if payload.get("observed_result") != entry.get("expected_violation"):
                violations.append((trace_file, "observed_result must match expected_violation"))
    return violations


def main() -> int:
    violations = validate_manifest()
    if violations:
        for subject, reason in violations:
            print(f"{subject}: {reason}", file=sys.stderr)
        return 1
    print("Lifecycle corpus validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
