#!/usr/bin/env python3
"""Run and validate the resource-envelope fixture matrix.

Large PDFs stay outside the repository. A qualification run should use a
manifest with exact fixture digests and sizes; the legacy ``--fixture`` form is
kept for exploratory runs and is intentionally not sufficient for ``--strict``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from scripts.resource_envelope.validate_envelope import validate_envelope


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUDGETS = ROOT / "docs" / "RESOURCE_ENVELOPE_BUDGETS.json"
MATRIX_KIND = "loupe-resource-envelope-matrix"
DEFAULT_RASTERIZERS = 8

# These names mirror issue #242. multi-gb is optional because platform
# addressability and available disk are environment-dependent.
FIXTURE_SPECS: dict[str, dict[str, Any]] = {
    "office-2mb": {"required": True, "expected_page_count": None, "workload": None, "min_bytes": 1_500_000, "max_bytes": 2_500_000},
    "image-heavy-500mb": {"required": True, "expected_page_count": None, "workload": None, "min_bytes": 450_000_000, "max_bytes": 550_000_000},
    "multi-gb": {"required": False, "expected_page_count": None, "workload": None, "min_bytes": 1_000_000_000, "max_bytes": None},
    "ten-thousand-page": {"required": True, "expected_page_count": 10000, "workload": "div2k-image-heavy", "min_bytes": None, "max_bytes": None},
    "pathological-vector": {"required": True, "expected_page_count": 256, "workload": "pathological-vector", "min_bytes": None, "max_bytes": None},
    "transparency-spots": {"required": True, "expected_page_count": 256, "workload": None, "min_bytes": None, "max_bytes": None},
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


def _git_head() -> str:
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


def _candidate_identity() -> dict[str, Any]:
    local_sha = _git_head()
    environment_sha = next((os.environ.get(key, "").strip() for key in ("GITHUB_SHA", "GIT_COMMIT") if os.environ.get(key, "").strip()), "")
    return {
        "candidate_sha": local_sha or environment_sha,
        "source": "git-head" if local_sha else "environment-fallback",
        "environment_sha": environment_sha,
        "verified": bool(local_sha) and (not environment_sha or environment_sha == local_sha),
    }


def _run_benchmark_process(command: list[str], timeout_seconds: float, cancel_after_seconds: float | None) -> subprocess.CompletedProcess[str]:
    creationflags = 0
    popen_kwargs: dict[str, Any] = {}
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        popen_kwargs["start_new_session"] = True
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        creationflags=creationflags,
        **popen_kwargs,
    )
    started = time.monotonic()
    if cancel_after_seconds is not None:
        while process.poll() is None and time.monotonic() - started < cancel_after_seconds:
            time.sleep(min(0.05, cancel_after_seconds - (time.monotonic() - started)))
        if process.poll() is None:
            if os.name == "nt":
                process.send_signal(getattr(signal, "CTRL_BREAK_EVENT", signal.SIGTERM))
            else:
                process.send_signal(signal.SIGINT)
    try:
        stdout, stderr = process.communicate(timeout=max(0.1, timeout_seconds - (time.monotonic() - started)))
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def _empty_result(fixture_id: str, reason: str) -> dict[str, Any]:
    return {
        "fixture_id": fixture_id,
        "status": "unavailable",
        "reason": reason,
        "result": None,
        "runs": [],
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
    if current.get("fixture_sha256") != baseline.get("fixture_sha256"):
        return ["baseline fixture digest does not match current fixture"]
    current_identity = current.get("identity")
    baseline_identity = baseline.get("identity")
    if isinstance(current_identity, Mapping) and isinstance(baseline_identity, Mapping):
        for key in ("os", "qt", "compiler", "renderer"):
            if current_identity.get(key) != baseline_identity.get(key):
                return [f"baseline identity mismatch: {key}"]
    errors: list[str] = []
    for field in ("rss_high_water_bytes", "elapsed_ms"):
        value = current.get(field)
        old_value = baseline_result.get(field)
        if not isinstance(value, int) or value < 0 or not isinstance(old_value, int) or old_value <= 0:
            continue
        if value > old_value * margin:
            errors.append(f"{field} {value} exceeds baseline {old_value} by margin {margin:g}")
    return errors


def _fixture_metadata(fixture_id: str, fixture_path: Path, metadata: Mapping[str, Any] | None, require_provenance: bool) -> tuple[dict[str, Any], list[str]]:
    spec = FIXTURE_SPECS[fixture_id]
    size = fixture_path.stat().st_size
    digest = _sha256(fixture_path)
    details: dict[str, Any] = {"fixture_sha256": digest, "input_bytes": size}
    errors: list[str] = []
    if metadata is None:
        if require_provenance:
            errors.append("fixture provenance manifest not supplied")
    else:
        expected_digest = metadata.get("sha256")
        expected_size = metadata.get("size_bytes")
        if expected_digest != digest:
            errors.append("fixture SHA-256 does not match manifest")
        if expected_size != size:
            errors.append(f"fixture size {size} does not match manifest {expected_size}")
        details["provenance"] = metadata.get("provenance", "")
        details["manifest_sha256"] = expected_digest
    minimum = spec.get("min_bytes")
    maximum = spec.get("max_bytes")
    if minimum is not None and size < minimum:
        errors.append(f"fixture is smaller than {minimum} bytes for {fixture_id}")
    if maximum is not None and size > maximum:
        errors.append(f"fixture is larger than {maximum} bytes for {fixture_id}")
    return details, errors


def _aggregate_envelopes(envelopes: list[Mapping[str, Any]]) -> tuple[dict[str, Any], dict[str, Any]]:
    # Use the highest-RSS run as the safety representative and the median
    # elapsed time. This keeps peak-memory validation conservative while
    # reducing scheduler noise in timing comparisons.
    representative = dict(max(envelopes, key=lambda item: item.get("rss_high_water_bytes", -1)))
    elapsed = [item["elapsed_ms"] for item in envelopes if isinstance(item.get("elapsed_ms"), int) and item["elapsed_ms"] >= 0]
    rss = [item["rss_high_water_bytes"] for item in envelopes if isinstance(item.get("rss_high_water_bytes"), int) and item["rss_high_water_bytes"] >= 0]
    stats = {
        "repetitions": len(envelopes),
        "elapsed_ms": {"median": statistics.median(elapsed) if elapsed else -1, "min": min(elapsed) if elapsed else -1, "max": max(elapsed) if elapsed else -1},
        "rss_high_water_bytes": {"median": statistics.median(rss) if rss else -1, "min": min(rss) if rss else -1, "max": max(rss) if rss else -1},
        "unstable": bool(rss and statistics.median(rss) > 0 and max(rss) > statistics.median(rss) * 1.2),
    }
    if elapsed:
        representative["elapsed_ms"] = int(statistics.median(elapsed))
    if rss:
        representative["rss_high_water_bytes"] = max(rss)
    return representative, stats


def run_fixture(
    pdf_tool: Path,
    fixture_id: str,
    fixture_path: Path,
    budgets: Mapping[str, Any],
    timeout_seconds: float,
    baseline: Mapping[str, Any] | None = None,
    margin: float = 2.0,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    metadata: Mapping[str, Any] | None = None,
    repetitions: int = 1,
    rasterizers: int = DEFAULT_RASTERIZERS,
    require_provenance: bool = False,
    cancel_after_seconds: float | None = None,
) -> dict[str, Any]:
    if repetitions < 1 or rasterizers < 1:
        raise ValueError("repetitions and rasterizers must be positive")
    # Resolve relative paths before the child is launched with cwd=ROOT.
    # Otherwise a relative fixture or tool path checked against the caller
    # directory would be looked up again relative to ROOT in the child.
    pdf_tool = Path(pdf_tool).resolve()
    fixture_path = Path(fixture_path).resolve()
    spec = FIXTURE_SPECS[fixture_id]
    fixture_details, provenance_errors = _fixture_metadata(fixture_id, fixture_path, metadata, require_provenance)
    # Pin rasterizers to a fixed value (8) so the same code and fixtures
    # produce comparable RSS and elapsed time across hosts with different
    # CPU counts. The value is recorded in the result profile.
    command = [str(pdf_tool), "benchmark", str(fixture_path), "--render-hw-accel", "0", "--render-rasterizers", str(rasterizers), "--console-format", "json"]
    record: dict[str, Any] = {
        "fixture_id": fixture_id,
        "path": str(fixture_path.resolve()),
        "expected_page_count": metadata.get("page_count", spec["expected_page_count"]) if metadata else spec["expected_page_count"],
        "workload": spec["workload"],
        "profile": {"render_hw_accel": False, "render_rasterizers": rasterizers},
        "command": command,
        **fixture_details,
    }
    if provenance_errors:
        record.update({"status": "failed", "result": None, "runs": [], "validation_errors": provenance_errors, "regressions": []})
        return record

    runs: list[dict[str, Any]] = []
    envelopes: list[Mapping[str, Any]] = []
    for index in range(repetitions):
        try:
            if runner is subprocess.run and cancel_after_seconds is not None:
                completed = _run_benchmark_process(command, timeout_seconds, cancel_after_seconds)
            else:
                completed = runner(command, cwd=ROOT, check=False, capture_output=True, text=True, timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            runs.append({"run": index + 1, "status": "unavailable", "reason": "benchmark-timeout", "process_exit_code": None})
            continue
        except OSError as exc:
            runs.append({"run": index + 1, "status": "unavailable", "reason": f"benchmark-launch-failed:{exc}", "process_exit_code": None})
            continue
        payload = _extract_json(completed.stdout)
        envelope = _envelope_from_output(payload) if payload else None
        if envelope is None:
            runs.append({"run": index + 1, "status": "unavailable", "reason": "benchmark-envelope-missing", "process_exit_code": completed.returncode, "stderr": completed.stderr[-2000:]})
            continue
        envelopes.append(envelope)
        runs.append({"run": index + 1, "status": "recorded", "process_exit_code": completed.returncode, "result": envelope})

    if not envelopes:
        record.update({"status": "unavailable", "result": None, "runs": runs, "validation_errors": [], "regressions": []})
        return record

    representative, stats = _aggregate_envelopes(envelopes)
    validation_errors: list[str] = []
    for index, envelope in enumerate(envelopes, start=1):
        for error in validate_envelope(envelope, budgets, spec["workload"]):
            validation_errors.append(f"run {index}: {error}")
        expected_page_count = record["expected_page_count"]
        if expected_page_count is not None and envelope.get("page_count") != expected_page_count:
            validation_errors.append(f"run {index}: page_count {envelope.get('page_count')} does not match expected {expected_page_count}")
        rss = envelope.get("rss_high_water_bytes")
        resident_limit = budgets.get("resource_budget", {}).get("resident_limit_bytes")
        if isinstance(rss, int) and rss >= 0 and isinstance(resident_limit, int) and rss > resident_limit:
            validation_errors.append(f"run {index}: RSS {rss} exceeds resident policy {resident_limit}")
    record["identity"] = representative.get("identity", {})
    record["result"] = representative
    record["statistics"] = stats
    record["runs"] = runs
    unavailable_runs = [run for run in runs if run["status"] != "recorded"]
    validation_errors.extend(
        f"run {run['run']}: {run['reason']}" for run in unavailable_runs
    )
    record["validation_errors"] = sorted(set(validation_errors))
    comparison = dict(representative)
    comparison["fixture_sha256"] = record["fixture_sha256"]
    comparison["identity"] = record["identity"]
    record["regressions"] = _regressions(comparison, baseline, margin)
    if cancel_after_seconds is not None:
        if representative.get("status") != "cancelled":
            validation_errors.append("cancellation probe did not produce a cancelled envelope")
        if not isinstance(representative.get("cancellation_latency_ms"), int) or representative["cancellation_latency_ms"] < 0:
            validation_errors.append("cancellation probe did not report cancellation latency")
        record["cancellation_probe"] = {"requested_after_seconds": cancel_after_seconds}
        record["validation_errors"] = sorted(set(validation_errors))
    hard_error_markers = ("does not match", "exceeds", "identity", "fixture SHA", "manifest")
    hard_errors = [error for error in record["validation_errors"] if any(marker in error for marker in hard_error_markers)]
    if record["regressions"] or hard_errors:
        record["status"] = "failed"
    elif record["validation_errors"] or representative.get("status") != "complete":
        record["status"] = "flagged"
    else:
        record["status"] = "measured"
    return record


def _load_fixture_manifest(path: Path) -> dict[str, dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_kind") != "loupe-resource-envelope-fixtures" or payload.get("schema_version") != 1:
        raise ValueError("fixture manifest schema_kind/schema_version is invalid")
    records = payload.get("fixtures")
    if not isinstance(records, list):
        raise ValueError("fixture manifest must contain a fixtures array")
    result: dict[str, dict[str, Any]] = {}
    for record in records:
        if not isinstance(record, dict) or record.get("fixture_id") not in FIXTURE_SPECS:
            raise ValueError("fixture manifest contains an unknown fixture_id")
        fixture_id = str(record["fixture_id"])
        if fixture_id in result:
            raise ValueError(f"fixture manifest contains duplicate fixture_id: {fixture_id}")
        if not isinstance(record.get("path"), str) or not record["path"]:
            raise ValueError(f"fixture manifest path missing: {fixture_id}")
        digest = record.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
            raise ValueError(f"fixture manifest sha256 missing: {fixture_id}")
        if not isinstance(record.get("size_bytes"), int) or record["size_bytes"] < 1:
            raise ValueError(f"fixture manifest size_bytes missing: {fixture_id}")
        if not isinstance(record.get("provenance"), str) or not record["provenance"].strip():
            raise ValueError(f"fixture manifest provenance missing: {fixture_id}")
        if "page_count" in record and (not isinstance(record["page_count"], int) or record["page_count"] < 1):
            raise ValueError(f"fixture manifest page_count is invalid: {fixture_id}")
        normalized = dict(record)
        fixture_path = Path(str(record["path"]))
        if not fixture_path.is_absolute():
            normalized["path"] = str((path.parent / fixture_path).resolve())
        result[fixture_id] = normalized
    return result


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


def run_matrix(
    pdf_tool: Path,
    fixtures: Mapping[str, Path | Mapping[str, Any]],
    budgets: Mapping[str, Any],
    timeout_seconds: float,
    baseline: Mapping[str, Any] | Path | None = None,
    margin: float = 2.0,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    repetitions: int = 1,
    rasterizers: int = DEFAULT_RASTERIZERS,
    cancel_fixture: str | None = None,
    cancel_after_seconds: float | None = None,
) -> dict[str, Any]:
    pdf_tool = Path(pdf_tool).resolve()
    baseline_by_fixture = _baseline_records(baseline) if isinstance(baseline, Path) else (baseline or {})
    identity = _candidate_identity()
    candidate_sha = identity["candidate_sha"]
    records: list[dict[str, Any]] = []
    for fixture_id, spec in FIXTURE_SPECS.items():
        supplied = fixtures.get(fixture_id)
        if supplied is None:
            record = _empty_result(fixture_id, "fixture-not-supplied" if spec["required"] else "fixture-not-supplied-optional")
            record["required"] = spec["required"]
            records.append(record)
            continue
        metadata = dict(supplied) if isinstance(supplied, Mapping) else None
        fixture_path = Path(metadata["path"]) if metadata else Path(supplied)
        fixture_path = fixture_path.resolve()
        if not fixture_path.is_file():
            record = _empty_result(fixture_id, "fixture-not-found")
            record["required"] = spec["required"]
            records.append(record)
            continue
        record = run_fixture(pdf_tool, fixture_id, fixture_path, budgets, timeout_seconds, baseline_by_fixture.get(fixture_id), margin, runner, metadata, repetitions, rasterizers, bool(metadata), cancel_after_seconds if fixture_id == cancel_fixture else None)
        record["required"] = spec["required"]
        result = record.get("result")
        if isinstance(result, Mapping):
            result_identity = result.get("identity")
            if isinstance(result_identity, Mapping):
                commit = result_identity.get("commit")
                if commit != candidate_sha:
                    record["validation_errors"] = sorted(set(record.get("validation_errors", []) + ["PdfTool identity commit does not match checkout HEAD"]))
                    record["status"] = "failed"
                expected_digest = record.get("fixture_sha256")
                fixture_digest = result_identity.get("fixture_digest")
                if expected_digest and fixture_digest != expected_digest:
                    record["validation_errors"] = sorted(set(record.get("validation_errors", []) + ["PdfTool identity fixture digest does not match input SHA-256"]))
                    record["status"] = "failed"
        records.append(record)

    failed = sum(record["status"] == "failed" for record in records)
    flagged = sum(record["required"] and record["status"] in {"flagged", "unavailable"} for record in records)
    skipped = sum(not record["required"] and record["status"] == "unavailable" for record in records)
    return {
        "schema_kind": MATRIX_KIND,
        "schema_version": 2,
        "candidate_sha": identity["candidate_sha"],
        "candidate_identity": identity,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "fixtures": records,
        "summary": {
            "total": len(records),
            "measured": sum(record["status"] == "measured" for record in records),
            "flagged": flagged,
            "skipped": skipped,
            "failed": failed,
            "candidate_sha_verified": identity["verified"],
        },
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pdf-tool", type=Path, required=True)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--manifest", type=Path)
    source.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--budgets", type=Path, default=DEFAULT_BUDGETS)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--margin", type=float, default=2.0)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--rasterizers", type=int, default=DEFAULT_RASTERIZERS)
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    parser.add_argument("--cancel-fixture", choices=tuple(FIXTURE_SPECS))
    parser.add_argument("--cancel-after-seconds", type=float)
    parser.add_argument("--strict", action="store_true", help="fail when required fixtures, provenance, or measurements are unavailable")
    args = parser.parse_args(argv)
    try:
        if args.margin <= 0 or args.timeout_seconds <= 0 or args.repetitions < 1 or args.rasterizers < 1:
            raise ValueError("margin, timeout-seconds, repetitions, and rasterizers must be positive")
        if args.strict and args.manifest is None:
            raise ValueError("--strict requires a fixture --manifest with exact digests and sizes")
        if (args.cancel_fixture is None) != (args.cancel_after_seconds is None):
            raise ValueError("--cancel-fixture and --cancel-after-seconds must be supplied together")
        if args.cancel_after_seconds is not None and args.cancel_after_seconds <= 0:
            raise ValueError("cancel-after-seconds must be positive")
        if args.cancel_fixture is not None and args.repetitions != 1:
            raise ValueError("cancellation probes require --repetitions 1")
        fixtures: Mapping[str, Path | Mapping[str, Any]] = _load_fixture_manifest(args.manifest) if args.manifest else _fixture_args(args.fixture)
        budgets = json.loads(args.budgets.read_text(encoding="utf-8"))
        matrix = run_matrix(args.pdf_tool, fixtures, budgets, args.timeout_seconds, args.baseline, args.margin, repetitions=args.repetitions, rasterizers=args.rasterizers, cancel_fixture=args.cancel_fixture, cancel_after_seconds=args.cancel_after_seconds)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(matrix, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"resource-envelope matrix error: {exc}", file=sys.stderr)
        return 2

    print(json.dumps(matrix["summary"], indent=2))
    return 1 if matrix["summary"]["failed"] or args.strict and (matrix["summary"]["flagged"] or not matrix["summary"]["candidate_sha_verified"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
