#!/usr/bin/env python3
"""Validate resource-envelope qualification evidence.

Schema version 1 is the frozen Session 11 record and can never claim
``passed``. Schema version 2 is built from the hosted qualification workflow
(``build_resource_envelope_evidence.py``) and may claim ``passed`` only when
every required platform measured every required fixture, the cancellation
and recovery probe, and the hostile corpus on one candidate SHA.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.resource_envelope.run_matrix import FIXTURE_SPECS


DEFAULT_EVIDENCE = ROOT / "docs" / "evidence" / "session-11-resource-envelope" / "evidence.json"
DEFAULT_MANIFEST = ROOT / "docs" / "evidence" / "session-11-resource-envelope" / "fixture-manifest.json"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_FIXTURES = tuple(
    fixture_id for fixture_id, spec in FIXTURE_SPECS.items() if spec["required"]
)
REQUIRED_PLATFORMS = ("linux", "windows")


def validate_evidence(
    evidence: dict[str, Any],
    *,
    manifest: dict[str, Any] | None = None,
) -> list[str]:
    if evidence.get("schema_kind") != "loop-resource-envelope-qualification-evidence":
        return ["schema_kind must be loop-resource-envelope-qualification-evidence"]
    if evidence.get("schema_version") == 2:
        return _validate_hosted_evidence(evidence)
    if evidence.get("schema_version") != 1:
        return ["schema_version must be 1 or 2"]
    return _validate_session_11_evidence(evidence, manifest)


def _is_measured(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _validate_platform(platform: str, record: Any, candidate_sha: Any) -> list[str]:
    if not isinstance(record, dict):
        return [f"platforms.{platform} must be an object"]
    errors: list[str] = []
    if record.get("candidate_sha") != candidate_sha:
        errors.append(f"platforms.{platform}.candidate_sha must equal candidate_sha")
    if record.get("strict_passed") is not True:
        errors.append(f"platforms.{platform} did not pass strict qualification")
    if not isinstance(record.get("matrix_sha256"), str) or not SHA256_RE.fullmatch(record["matrix_sha256"]):
        errors.append(f"platforms.{platform}.matrix_sha256 must be a SHA-256 digest")
    fixtures = record.get("fixtures") if isinstance(record.get("fixtures"), dict) else {}
    for fixture_id in REQUIRED_FIXTURES:
        entry = fixtures.get(fixture_id)
        envelope = entry.get("envelope") if isinstance(entry, dict) else None
        if not isinstance(entry, dict) or entry.get("status") != "measured" or not isinstance(envelope, dict):
            errors.append(f"platforms.{platform}.fixtures.{fixture_id} is not measured")
            continue
        if envelope.get("status") != "complete":
            errors.append(f"platforms.{platform}.fixtures.{fixture_id} envelope is not complete")
        for field in ("rss_high_water_bytes", "preflight_high_water_bytes", "elapsed_ms"):
            if not _is_measured(envelope.get(field)):
                errors.append(f"platforms.{platform}.fixtures.{fixture_id}.{field} is unavailable")
    probe = record.get("cancellation_recovery_probe") if isinstance(record.get("cancellation_recovery_probe"), dict) else {}
    if probe.get("status") != "measured":
        errors.append(f"platforms.{platform} cancellation/recovery probe is not measured")
    for field in ("cancellation_latency_ms", "recovery_ms"):
        if not _is_measured(probe.get(field)):
            errors.append(f"platforms.{platform}.cancellation_recovery_probe.{field} is unavailable")
    hostile = record.get("hostile") if isinstance(record.get("hostile"), dict) else {}
    summary = hostile.get("summary") if isinstance(hostile.get("summary"), dict) else {}
    if not _is_measured(summary.get("total")) or summary.get("total") == 0 or summary.get("contained") != summary.get("total"):
        errors.append(f"platforms.{platform} hostile corpus was not fully contained")
    return errors


def _validate_hosted_evidence(evidence: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    candidate_sha = evidence.get("candidate_sha")
    if not isinstance(candidate_sha, str) or not SHA_RE.fullmatch(candidate_sha):
        errors.append("candidate_sha must be a 40-character lowercase commit SHA")
    if not isinstance(evidence.get("fixture_manifest_sha256"), str) or not SHA256_RE.fullmatch(evidence["fixture_manifest_sha256"]):
        errors.append("fixture_manifest_sha256 must be a SHA-256 digest")
    disposition = evidence.get("disposition")
    if disposition not in {"incomplete", "passed", "rejected"}:
        errors.append("disposition must be incomplete, passed, or rejected")
    reasons = evidence.get("disposition_reasons")
    if not isinstance(reasons, list):
        errors.append("disposition_reasons must be an array")
    elif disposition == "passed" and reasons:
        errors.append("passed evidence cannot carry disposition_reasons")
    elif disposition != "passed" and not reasons:
        errors.append("non-passed evidence must state its disposition_reasons")

    runs = evidence.get("ci_runs")
    if not isinstance(runs, list) or not runs:
        errors.append("ci_runs must be a non-empty array")
    else:
        for index, run in enumerate(runs):
            if not isinstance(run, dict) or not str(run.get("run_id", "")).isdigit():
                errors.append(f"ci_runs[{index}].run_id must be a GitHub Actions run id")
                continue
            if not str(run.get("run_url", "")).startswith("https://github.com/"):
                errors.append(f"ci_runs[{index}].run_url must be a GitHub Actions run URL")
            if run.get("candidate_sha") != candidate_sha:
                errors.append(f"ci_runs[{index}].candidate_sha must equal candidate_sha")

    platforms = evidence.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        errors.append("platforms must be a non-empty object")
        return errors
    if disposition == "passed":
        for platform in REQUIRED_PLATFORMS:
            if platform not in platforms:
                errors.append(f"passed evidence is missing platform {platform}")
        for platform, record in platforms.items():
            errors.extend(_validate_platform(platform, record, candidate_sha))
    return errors


def _validate_session_11_evidence(evidence: dict[str, Any], manifest: dict[str, Any] | None) -> list[str]:
    errors: list[str] = []

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
