#!/usr/bin/env python3
"""Run and validate the resource-envelope fixture matrix.

The matrix is deliberately an external-fixture runner. Large PDFs are not
checked into the repository; callers provide their paths with
``--fixture NAME=PATH``. Every attempt is still recorded, including a missing
fixture, a timeout, an unavailable workload, or an incomplete PdfTool record.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from scripts.resource_envelope.validate_envelope import validate_envelope


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUDGETS = ROOT / "docs" / "RESOURCE_ENVELOPE_BUDGETS.json"
MATRIX_KIND = "loupe-resource-envelope-matrix"

# These names mirror issue #242. multi-gb is optional because platform
# addressability and available disk are environment-dependent.
FIXTURE_SPECS: dict[str, dict[str, Any]] = {
    "office-2mb": {"required": True, "expected_page_count": None, "workload": None},
    "image-heavy-500mb": {"required": True, "expected_page_count": None, "workload": None},
    "multi-gb": {"required": False, "expected_page_count": None, "workload": None},
    "ten-thousand-page": {"required": True, "expected_page_count": 10000, "workload": "div2k-image-heavy"},
    "pathological-vector": {"required": True, "expected_page_count": 256, "workload": "pathological-vector"},
    "transparency-spots": {"required": True, "expected_page_count": 256, "workload": None},
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _extract_json(stdout: str) -> dict[str, Any] | None:
    """Extract PdfTool's JSON object, tolerating diagnostic text on stdout."""
    decoder = json.JSONDecoder()
    for index, character in enumerate(stdout):
        if character != "{":
            continue
        try:
            value, _ = decoder.raw_decode(stdout[index:])
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    return None


def _envelope_from_output(payload: Mapping[str, Any]) -> dict[str, Any] | None:
    data = payload.get("data")
    if isinstance(data, Mapping) and isinstance(data.get("workload_envelope"), Mapping):
        return dict(data["workload_envelope"])
    if isinstance(payload.get("workload_envelope"), Mapping):
        return dict(payload["workload_envelope"])
    return None


def _candidate_sha() -> str:
    for key in ("GITHUB_SHA", "GIT_COMMIT"):
        value = os.environ.get(key, "").strip()
        if value:
            return value
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def _empty_result(fixture_id: str, reason: str) -> dict[str, Any]:
    return {
        "fixture_id": fixture_id,
        "status": "unavailable",
        "reason": reason,
        "result": None,
        "validation_errors": [],
        "regressions": [],
    }


def _baseline_records(path: Path | None) -> dict[str, Mapping[str, Any]]:
    if path is None:
        return {}
    payload = json.loads(path.read_text(encoding="utf-8"))
    records = payload.get("fixtures")
    if not isinstance(records, list):
        raise ValueError("baseline must contain a fixtures array")
    return {
        str(record["fixture_id"]): record
        for record in records
        if isinstance(record, Mapping) and "fixture_id" in record
    }


def _regressions(current: Mapping[str, Any], baseline: Mapping[str, Any] | None, margin: float) -> list[str]:
    if baseline is None:
        return []
    baseline_result = baseline.get("result")
    if not isinstance(baseline_result, Mapping):
        return []
    errors: list[str] = []
    for field in ("rss_high_water_bytes", "elapsed_ms"):
        value = current.get(field)
        old_value = baseline_result.get(field)
        if not isinstance(value, int) or value < 0 or not isinstance(old_value, int) or old_value <= 0:
            continue
        if value > old_value * margin:
            errors.append(f"{field} {value} exceeds baseline {old_value} by margin {margin:g}")
    return errors


def run_fixture(
    pdf_tool: Path,
    fixture_id: str,
    fixture_path: Path,
    budgets: Mapping[str, Any],
    timeout_seconds: float,
    baseline: Mapping[str, Any] | None = None,
    margin: float = 2.0,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict[str, Any]:
    spec = FIXTURE_SPECS[fixture_id]
    record: dict[str, Any] = {
        "fixture_id": fixture_id,
        "path": str(fixture_path.resolve()),
        "input_bytes": fixture_path.stat().st_size,
        "input_sha256": _sha256(fixture_path),
        "expected_page_count": spec["expected_page_count"],
        "workload": spec["workload"],
        "command": [str(pdf_tool), "benchmark", str(fixture_path), "--render-hw-accel", "0", "--console-format", "json"],
    }
    try:
        completed = runner(
            record["command"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired:
        record.update(_empty_result(fixture_id, "benchmark-timeout"))
        record["process_exit_code"] = None
        return record
    except OSError as exc:
        record.update(_empty_result(fixture_id, f"benchmark-launch-failed:{exc}"))
        record["process_exit_code"] = None
        return record

    record["process_exit_code"] = completed.returncode
    payload = _extract_json(completed.stdout)
    envelope = _envelope_from_output(payload) if payload else None
    if envelope is None:
        record.update(_empty_result(fixture_id, "benchmark-envelope-missing"))
        record["stderr"] = completed.stderr[-2000:]
        return record

    validation_errors = validate_envelope(envelope, budgets, spec["workload"])
    page_count = envelope.get("page_count")
    expected_page_count = spec["expected_page_count"]
    if expected_page_count is not None and page_count != expected_page_count:
        validation_errors.append(f"page_count {page_count} does not match expected {expected_page_count}")
    regressions = _regressions(envelope, baseline, margin)
    status = "failed" if validation_errors or regressions else "measured"
    if envelope.get("status") != "complete" and status == "measured":
        status = "flagged"
    record.update({
        "status": status,
        "result": envelope,
        "validation_errors": validation_errors,
        "regressions": regressions,
    })
    return record


def run_matrix(
    pdf_tool: Path,
    fixtures: Mapping[str, Path],
    budgets: Mapping[str, Any],
    timeout_seconds: float,
    baseline: Mapping[str, Any] | None = None,
    margin: float = 2.0,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict[str, Any]:
    baseline_by_fixture = _baseline_records(baseline) if isinstance(baseline, Path) else (baseline or {})
    records: list[dict[str, Any]] = []
    for fixture_id, spec in FIXTURE_SPECS.items():
        fixture_path = fixtures.get(fixture_id)
        if fixture_path is None:
            if spec["required"]:
                record = _empty_result(fixture_id, "fixture-not-supplied")
            else:
                record = _empty_result(fixture_id, "fixture-not-supplied-optional")
            record["required"] = spec["required"]
            records.append(record)
            continue
        if not fixture_path.is_file():
            record = _empty_result(fixture_id, "fixture-not-found")
            record["required"] = spec["required"]
            records.append(record)
            continue
        record = run_fixture(
            pdf_tool,
            fixture_id,
            fixture_path,
            budgets,
            timeout_seconds,
            baseline_by_fixture.get(fixture_id),
            margin,
            runner,
        )
        record["required"] = spec["required"]
        records.append(record)

    failed = sum(record["status"] == "failed" for record in records)
    flagged = sum(
        record["required"] and record["status"] in {"flagged", "unavailable"}
        for record in records
    )
    skipped = sum(
        not record["required"] and record["status"] == "unavailable"
        for record in records
    )
    return {
        "schema_kind": MATRIX_KIND,
        "schema_version": 1,
        "candidate_sha": _candidate_sha(),
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "fixtures": records,
        "summary": {
            "total": len(records),
            "measured": sum(record["status"] == "measured" for record in records),
            "flagged": flagged,
            "skipped": skipped,
            "failed": failed,
        },
    }


def _fixture_args(values: Sequence[str]) -> dict[str, Path]:
    fixtures: dict[str, Path] = {}
    for value in values:
        name, separator, path = value.partition("=")
        if not separator or name not in FIXTURE_SPECS or not path:
            raise ValueError(f"fixture must be NAME=PATH for one of: {', '.join(FIXTURE_SPECS)}")
        if name in fixtures:
            raise ValueError(f"fixture supplied more than once: {name}")
        fixtures[name] = Path(path)
    return fixtures


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pdf-tool", type=Path, required=True)
    parser.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--budgets", type=Path, default=DEFAULT_BUDGETS)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--margin", type=float, default=2.0)
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    parser.add_argument("--strict", action="store_true", help="fail when required fixtures are unavailable or flagged")
    args = parser.parse_args(argv)
    try:
        if args.margin <= 0 or args.timeout_seconds <= 0:
            raise ValueError("margin and timeout-seconds must be positive")
        fixtures = _fixture_args(args.fixture)
        budgets = json.loads(args.budgets.read_text(encoding="utf-8"))
        matrix = run_matrix(args.pdf_tool, fixtures, budgets, args.timeout_seconds, args.baseline, args.margin)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(matrix, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"resource-envelope matrix error: {exc}", file=sys.stderr)
        return 2

    print(json.dumps(matrix["summary"], indent=2))
    return 1 if matrix["summary"]["failed"] or args.strict and matrix["summary"]["flagged"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
