#!/usr/bin/env python3
"""Validate frozen Session 11 resource-envelope qualification evidence."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

from scripts.resource_envelope.run_matrix import FIXTURE_SPECS


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EVIDENCE = ROOT / "docs" / "evidence" / "session-11-resource-envelope" / "evidence.json"
DEFAULT_MANIFEST = ROOT / "docs" / "evidence" / "session-11-resource-envelope" / "fixture-manifest.json"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_FIXTURES = tuple(
    fixture_id for fixture_id, spec in FIXTURE_SPECS.items() if spec["required"]
)


def validate_evidence(
    evidence: dict[str, Any],
    *,
    manifest: dict[str, Any] | None = None,
) -> list[str]:
    errors: list[str] = []
    if evidence.get("schema_kind") != "loop-resource-envelope-qualification-evidence":
        errors.append("schema_kind must be loop-resource-envelope-qualification-evidence")
    if evidence.get("schema_version") != 1:
        errors.append("schema_version must be 1")

    candidate_sha = evidence.get("candidate_sha")
    if not isinstance(candidate_sha, str) or not SHA_RE.fullmatch(candidate_sha):
        errors.append("candidate_sha must be a 40-character lowercase commit SHA")

    disposition = evidence.get("disposition")
    if disposition not in {"incomplete", "passed", "rejected"}:
        errors.append("disposition must be incomplete, passed, or rejected")
    if disposition == "passed":
        errors.append("Session 11 evidence must not claim passed until hosted matrix is complete")

    fixture_status = evidence.get("fixture_status")
    if not isinstance(fixture_status, dict):
        errors.append("fixture_status must be an object")
        return errors

    for fixture_id in REQUIRED_FIXTURES:
        entry = fixture_status.get(fixture_id)
        if not isinstance(entry, dict):
            errors.append(f"fixture_status missing required fixture: {fixture_id}")
            continue
        status = entry.get("status")
        if status not in {"measured", "flagged", "failed", "unavailable", "incomplete"}:
            errors.append(f"fixture_status.{fixture_id}.status is invalid")
        if status in {"measured", "passed"} and entry.get("preflight_high_water_bytes") == -1:
            errors.append(f"fixture_status.{fixture_id} cannot pass with unavailable preflight")

    runs = evidence.get("matrix_runs")
    if not isinstance(runs, list) or not runs:
        errors.append("matrix_runs must be a non-empty array")
    else:
        for index, run in enumerate(runs):
            if not isinstance(run, dict):
                errors.append(f"matrix_runs[{index}] must be an object")
                continue
            run_sha = run.get("candidate_sha")
            if isinstance(candidate_sha, str) and run_sha != candidate_sha:
                errors.append(f"matrix_runs[{index}].candidate_sha must equal candidate_sha")

    if manifest is not None:
        records = manifest.get("fixtures")
        if not isinstance(records, list):
            errors.append("fixture manifest fixtures must be an array")

    verifiers = evidence.get("verifiers")
    if not isinstance(verifiers, list) or not verifiers:
        errors.append("verifiers must be a non-empty array")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=DEFAULT_EVIDENCE)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--skip-manifest", action="store_true")
    args = parser.parse_args(argv)

    try:
        evidence = json.loads(args.evidence.read_text(encoding="utf-8"))
        manifest = None if args.skip_manifest or not args.manifest.is_file() else json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"resource-envelope evidence validation error: {exc}", file=sys.stderr)
        return 2

    errors = validate_evidence(evidence, manifest=manifest)
    if errors:
        for error in errors:
            print(f"resource-envelope-evidence: {error}", file=sys.stderr)
        return 1

    print(json.dumps({"status": "valid", "candidate_sha": evidence.get("candidate_sha")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
