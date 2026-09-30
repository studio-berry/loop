#!/usr/bin/env python3
"""Build hosted resource-envelope qualification evidence from matrix results.

Each ``--matrix PLATFORM=PATH`` is one strict ``run_matrix.py`` output from the
hosted qualification workflow. The evidence records the exact CI run, candidate
SHA, fixture manifest digest, and per-fixture measurements for every platform.
It claims ``passed`` only when every required platform passed strict
qualification on the same candidate SHA.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.resource_envelope.run_matrix import matrix_passes


EVIDENCE_KIND = "loop-resource-envelope-qualification-evidence"
REQUIRED_PLATFORMS = ("linux", "windows")
MEASUREMENT_FIELDS = ("status", "rss_high_water_bytes", "preflight_high_water_bytes", "elapsed_ms", "pages_materialized", "page_count")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _platform_record(matrix: Mapping[str, Any], matrix_sha256: str) -> dict[str, Any]:
    fixtures: dict[str, Any] = {}
    for record in matrix["fixtures"]:
        result = record.get("result") if isinstance(record.get("result"), Mapping) else {}
        fixtures[record["fixture_id"]] = {
            "status": record["status"],
            "required": record.get("required", False),
            "fixture_sha256": record.get("fixture_sha256"),
            "workload": record.get("workload"),
            "envelope": {field: result.get(field) for field in MEASUREMENT_FIELDS} if result else None,
            "statistics": record.get("statistics"),
            "validation_errors": record.get("validation_errors", []),
        }
    probe = matrix.get("cancellation_recovery_probe") or {}
    hostile = matrix.get("hostile") or {}
    identity = next((record["identity"] for record in matrix["fixtures"] if isinstance(record.get("identity"), Mapping) and record["identity"]), {})
    return {
        "matrix_sha256": matrix_sha256,
        "candidate_sha": matrix["candidate_sha"],
        "strict_passed": matrix_passes(matrix, strict=True),
        "summary": matrix["summary"],
        "runtime": {key: identity.get(key) for key in ("os", "qt", "compiler", "cpu", "renderer", "build")},
        "fixtures": fixtures,
        "cancellation_recovery_probe": {
            "status": probe.get("status", "not-run"),
            "fixture_id": probe.get("fixture_id"),
            "cancellation_latency_ms": probe.get("cancellation", {}).get("cancellation_latency_ms", -1),
            "recovery_ms": probe.get("recovery", {}).get("recovery_ms", -1),
            "validation_errors": probe.get("validation_errors", []),
        },
        "hostile": {
            "summary": hostile.get("summary", {"total": 0, "contained": 0}),
            "failed_cases": [case["case_id"] for case in hostile.get("cases", []) if case.get("status") != "contained"],
        },
    }


def build_evidence(
    matrices: Mapping[str, Path],
    fixture_manifest: Path,
    run_id: str,
    run_url: str,
) -> dict[str, Any]:
    platforms: dict[str, Any] = {}
    for platform, path in sorted(matrices.items()):
        matrix = json.loads(path.read_text(encoding="utf-8"))
        platforms[platform] = _platform_record(matrix, _sha256(path))

    candidates = {record["candidate_sha"] for record in platforms.values()}
    reasons: list[str] = []
    missing = [platform for platform in REQUIRED_PLATFORMS if platform not in platforms]
    reasons.extend(f"platform {platform} produced no matrix" for platform in missing)
    if len(candidates) > 1:
        reasons.append(f"platforms measured different candidate SHAs: {sorted(candidates)}")
    for platform, record in platforms.items():
        if not record["strict_passed"]:
            reasons.append(f"platform {platform} did not pass strict qualification")

    rejected = any(
        record["summary"]["failed"] or record["cancellation_recovery_probe"]["status"] == "failed" or record["hostile"]["failed_cases"]
        for record in platforms.values()
    )
    disposition = "passed" if not reasons else ("rejected" if rejected else "incomplete")
    manifest = json.loads(fixture_manifest.read_text(encoding="utf-8"))
    return {
        "schema_kind": EVIDENCE_KIND,
        "schema_version": 2,
        "issue": 19,
        "candidate_sha": next(iter(candidates)) if len(candidates) == 1 else "",
        "disposition": disposition,
        "disposition_reasons": reasons,
        "fixture_manifest_sha256": _sha256(fixture_manifest),
        "fixture_generator": manifest.get("generator"),
        "ci_runs": [{"platform": platform, "run_id": run_id, "run_url": run_url, "candidate_sha": record["candidate_sha"]} for platform, record in platforms.items()],
        "platforms": platforms,
    }


def _matrix_args(values: Sequence[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        platform, separator, path = value.partition("=")
        if not separator or not platform or not path:
            raise ValueError("--matrix must be PLATFORM=PATH")
        result[platform] = Path(path)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", action="append", default=[], metavar="PLATFORM=PATH")
    parser.add_argument("--fixture-manifest", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        evidence = build_evidence(_matrix_args(args.matrix), args.fixture_manifest, args.run_id, args.run_url)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"resource-envelope evidence error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"disposition": evidence["disposition"], "reasons": evidence["disposition_reasons"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
