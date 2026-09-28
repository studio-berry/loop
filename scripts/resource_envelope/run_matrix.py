#!/usr/bin/env python3
"""Run and validate the resource-envelope fixture matrix.

Large PDFs stay outside the repository. A qualification run should use a
manifest with exact fixture digests and sizes; the legacy ``--fixture`` form is
intentionally not sufficient for ``--strict``.

Besides the measured fixtures, a strict run carries a cancellation probe that
interrupts one fixture, a recovery probe that times a fresh process reopening
it, and a hostile lane that feeds the checked-in budget-exhaustion PDFs to
PdfTool. A crash, timeout, or skipped workload never counts as a passing
envelope.
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
DEFAULT_PREFLIGHT_PROFILE = ROOT / "loop-preflight" / "profiles" / "loop-default.json"
DEFAULT_HOSTILE_CORPUS = ROOT / "UnitTests" / "testdata" / "budget_exhaustion"
MATRIX_KIND = "loop-resource-envelope-matrix"
DEFAULT_RASTERIZERS = 8
# PdfTool's defined terminal exit codes (pdftoolresult.h) except InternalError
# (7). Anything else, including a negative POSIX signal or a Windows exception
# status, means the process did not reach a controlled disposition.
CONTAINED_EXIT_CODES = frozenset({0, 1, 2, 3, 4, 5, 6, 8, 9})
EXIT_SUCCESS = 0
EXIT_CANCELLED = 6
# Validation errors carrying one of these markers fail the record outright;
# every other error only flags it.
HARD_ERROR_MARKERS = ("does not match", "exceeds", "identity", "fixture SHA", "manifest", "timeout", "crashed")

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

Runner = Callable[..., subprocess.CompletedProcess[str]]
CancelRunner = Callable[[list[str], float, float | None], subprocess.CompletedProcess[str]]


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


def _envelope_from_process(completed: subprocess.CompletedProcess[str]) -> dict[str, Any] | None:
    payload = _extract_json(completed.stdout or "")
    return _envelope_from_output(payload) if payload else None


def _find_key(value: Any, key: str) -> Any:
    if isinstance(value, Mapping):
        if key in value:
            return value[key]
        children: Sequence[Any] = list(value.values())
    elif isinstance(value, list):
        children = value
    else:
        return None
    for child in children:
        found = _find_key(child, key)
        if found is not None:
            return found
    return None


def _failure_detail(completed: subprocess.CompletedProcess[str]) -> str:
    """Short, log-sized account of why a PdfTool run did not succeed."""
    payload = _extract_json(completed.stdout or "")
    errors = _find_key(payload, "rendering-errors") if payload else None
    parts = []
    if errors is not None:
        parts.append("rendering-errors=" + json.dumps(errors)[:800])
    stderr = (completed.stderr or "").strip()
    if stderr:
        parts.append("stderr=" + stderr[-400:])
    return "; ".join(parts)


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


def _benchmark_command(
    pdf_tool: Path,
    fixture_path: Path,
    rasterizers: int,
    preflight_profile: Path | None,
    first_page_only: bool = False,
) -> list[str]:
    # Pin rasterizers to a fixed value (8) so the same code and fixtures
    # produce comparable RSS and elapsed time across hosts with different
    # CPU counts. The value is recorded in the result profile.
    command = [str(pdf_tool), "benchmark", str(fixture_path), "--render-hw-accel", "0", "--render-rasterizers", str(rasterizers), "--console-format", "json"]
    if preflight_profile is not None:
        command += ["--profile", str(preflight_profile)]
    if first_page_only:
        command += ["--page-first", "1", "--page-last", "1"]
    return command


def _identity_errors(envelope: Mapping[str, Any], candidate_sha: str, fixture_sha256: str) -> list[str]:
    identity = envelope.get("identity") if isinstance(envelope.get("identity"), Mapping) else {}
    errors: list[str] = []
    if identity.get("commit") != candidate_sha:
        errors.append(f"identity.commit {identity.get('commit')!r} does not match candidate {candidate_sha!r}")
    if identity.get("fixture_digest") != fixture_sha256:
        errors.append(f"identity.fixture_digest {identity.get('fixture_digest')!r} does not match input {fixture_sha256!r}")
    return errors


def _is_hard(error: str) -> bool:
    return any(marker in error for marker in HARD_ERROR_MARKERS)


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
        raise subprocess.TimeoutExpired(command, timeout_seconds, stdout, stderr)
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def _timed_run(runner: Runner, command: list[str], timeout_seconds: float) -> tuple[subprocess.CompletedProcess[str] | None, str, int]:
    """Runs one PdfTool process; returns (completed, failure reason, wall ms)."""
    started = time.monotonic()
    try:
        completed = runner(command, cwd=ROOT, check=False, capture_output=True, text=True, timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        return None, "benchmark-timeout", int((time.monotonic() - started) * 1000)
    except OSError as exc:
        return None, f"benchmark-launch-failed:{exc}", -1
    wall_ms = int((time.monotonic() - started) * 1000)
    if completed.returncode not in CONTAINED_EXIT_CODES:
        return completed, f"process-crashed:{completed.returncode}", wall_ms
    return completed, "", wall_ms


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


def _fixture_workload(fixture_id: str, metadata: Mapping[str, Any] | None) -> str | None:
    if metadata is not None and "workload" in metadata:
        return str(metadata["workload"])
    return FIXTURE_SPECS[fixture_id]["workload"]


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
    runner: Runner = subprocess.run,
    metadata: Mapping[str, Any] | None = None,
    repetitions: int = 1,
    rasterizers: int = DEFAULT_RASTERIZERS,
    require_provenance: bool = False,
    candidate_sha: str | None = None,
    preflight_profile: Path | None = None,
) -> dict[str, Any]:
    if repetitions < 1 or rasterizers < 1:
        raise ValueError("repetitions and rasterizers must be positive")
    if candidate_sha is None:
        candidate_sha = _candidate_identity()["candidate_sha"]
    # Resolve relative paths before the child is launched with cwd=ROOT.
    # Otherwise a relative fixture or tool path checked against the caller
    # directory would be looked up again relative to ROOT in the child.
    pdf_tool = Path(pdf_tool).resolve()
    fixture_path = Path(fixture_path).resolve()
    spec = FIXTURE_SPECS[fixture_id]
    workload = _fixture_workload(fixture_id, metadata)
    fixture_details, provenance_errors = _fixture_metadata(fixture_id, fixture_path, metadata, require_provenance)
    command = _benchmark_command(pdf_tool, fixture_path, rasterizers, preflight_profile)
    record: dict[str, Any] = {
        "fixture_id": fixture_id,
        "path": str(fixture_path),
        "expected_page_count": metadata.get("page_count", spec["expected_page_count"]) if metadata else spec["expected_page_count"],
        "workload": workload,
        "profile": {"render_hw_accel": False, "render_rasterizers": rasterizers, "preflight_profile": str(preflight_profile) if preflight_profile else None},
        "command": command,
        **fixture_details,
    }
    if provenance_errors:
        record.update({"status": "failed", "result": None, "runs": [], "validation_errors": provenance_errors, "regressions": []})
        return record

    runs: list[dict[str, Any]] = []
    envelopes: list[Mapping[str, Any]] = []
    validation_errors: list[str] = []
    for index in range(1, repetitions + 1):
        completed, failure, wall_ms = _timed_run(runner, command, timeout_seconds)
        envelope = _envelope_from_process(completed) if completed is not None else None
        exit_code = completed.returncode if completed is not None else None
        if failure or envelope is None:
            reason = failure or "benchmark-envelope-missing"
            run: dict[str, Any] = {"run": index, "status": "unavailable", "reason": reason, "process_exit_code": exit_code, "process_wall_ms": wall_ms}
            if completed is not None:
                run["stderr"] = (completed.stderr or "")[-2000:]
                run["detail"] = _failure_detail(completed)
            runs.append(run)
            validation_errors.append(f"run {index}: {reason}")
            continue
        envelopes.append(envelope)
        runs.append({"run": index, "status": "recorded", "process_exit_code": exit_code, "process_wall_ms": wall_ms, "result": envelope})
        if exit_code != EXIT_SUCCESS:
            runs[-1]["detail"] = _failure_detail(completed)
            validation_errors.append(f"run {index}: process exit code {exit_code} is not success")
        for error in validate_envelope(envelope, budgets, workload):
            validation_errors.append(f"run {index}: {error}")
        expected_page_count = record["expected_page_count"]
        if expected_page_count is not None and envelope.get("page_count") != expected_page_count:
            validation_errors.append(f"run {index}: page_count {envelope.get('page_count')} does not match expected {expected_page_count}")
        rss = envelope.get("rss_high_water_bytes")
        resident_limit = budgets.get("resource_budget", {}).get("resident_limit_bytes")
        if isinstance(rss, int) and rss >= 0 and isinstance(resident_limit, int) and rss > resident_limit:
            validation_errors.append(f"run {index}: RSS {rss} exceeds resident policy {resident_limit}")
        validation_errors.extend(f"run {index}: {error}" for error in _identity_errors(envelope, candidate_sha, record["fixture_sha256"]))

    record["runs"] = runs
    record["validation_errors"] = sorted(set(validation_errors))
    record["regressions"] = []
    if not envelopes:
        record["result"] = None
        record["status"] = "failed" if any(_is_hard(error) for error in record["validation_errors"]) else "unavailable"
        return record

    representative, stats = _aggregate_envelopes(envelopes)
    record["identity"] = representative.get("identity", {})
    record["result"] = representative
    record["statistics"] = stats
    comparison = dict(representative)
    comparison["fixture_sha256"] = record["fixture_sha256"]
    comparison["identity"] = record["identity"]
    record["regressions"] = _regressions(comparison, baseline, margin)
    if record["regressions"] or any(_is_hard(error) for error in record["validation_errors"]):
        record["status"] = "failed"
    elif record["validation_errors"] or any(envelope.get("status") != "complete" for envelope in envelopes):
        record["status"] = "flagged"
    else:
        record["status"] = "measured"
    return record


def run_cancellation_probe(
    pdf_tool: Path,
    fixture_id: str,
    fixture_path: Path,
    budgets: Mapping[str, Any],
    timeout_seconds: float,
    cancel_after_seconds: float,
    candidate_sha: str,
    workload: str | None = None,
    rasterizers: int = DEFAULT_RASTERIZERS,
    preflight_profile: Path | None = None,
    cancel_runner: CancelRunner = _run_benchmark_process,
    runner: Runner = subprocess.run,
) -> dict[str, Any]:
    """Interrupts one run, then times a fresh process reopening the fixture.

    ``recovery_ms`` is the wall time from launching that fresh process until it
    exits having rendered the first page: the time an operator waits to get the
    document back after abandoning a run.
    """
    pdf_tool = Path(pdf_tool).resolve()
    fixture_path = Path(fixture_path).resolve()
    fixture_sha256 = _sha256(fixture_path)
    limits = budgets.get("workloads", {}).get(workload, {}) if workload else {}
    errors: list[str] = []

    cancel_command = _benchmark_command(pdf_tool, fixture_path, rasterizers, preflight_profile)
    cancellation: dict[str, Any] = {"command": cancel_command, "requested_after_seconds": cancel_after_seconds, "cancellation_latency_ms": -1}
    try:
        completed = cancel_runner(cancel_command, timeout_seconds, cancel_after_seconds)
    except subprocess.TimeoutExpired:
        completed = None
        errors.append("cancellation probe timeout: the process did not stop after the interrupt")
    except OSError as exc:
        completed = None
        errors.append(f"cancellation probe launch failed: {exc}")
    if completed is not None:
        cancellation["process_exit_code"] = completed.returncode
        envelope = _envelope_from_process(completed)
        if completed.returncode not in CONTAINED_EXIT_CODES:
            errors.append(f"cancellation probe crashed with exit code {completed.returncode}")
        elif envelope is None:
            errors.append("cancellation probe produced no envelope")
        else:
            cancellation["result"] = envelope
            latency = envelope.get("cancellation_latency_ms")
            if envelope.get("status") != "cancelled":
                errors.append(f"cancellation probe ended {envelope.get('status')!r}; the interrupt arrived after the run finished or was ignored")
            if completed.returncode != EXIT_CANCELLED:
                errors.append(f"cancellation probe exit code {completed.returncode} is not Cancelled ({EXIT_CANCELLED})")
            if not isinstance(latency, int) or isinstance(latency, bool) or latency < 0:
                errors.append("cancellation probe did not report cancellation latency")
            else:
                cancellation["cancellation_latency_ms"] = latency
                limit = limits.get("cancellation_latency_ms")
                if isinstance(limit, int) and latency > limit:
                    errors.append(f"cancellation_latency_ms {latency} exceeds workload policy {limit}")
            errors.extend(_identity_errors(envelope, candidate_sha, fixture_sha256))

    recovery_command = _benchmark_command(pdf_tool, fixture_path, rasterizers, None, first_page_only=True)
    recovery: dict[str, Any] = {"command": recovery_command, "recovery_ms": -1}
    completed, failure, wall_ms = _timed_run(runner, recovery_command, timeout_seconds)
    if completed is not None:
        recovery["process_exit_code"] = completed.returncode
    if failure:
        errors.append(f"recovery probe {failure}")
    elif completed is not None:
        envelope = _envelope_from_process(completed)
        if envelope is None:
            errors.append("recovery probe produced no envelope")
        else:
            recovery["result"] = envelope
            if completed.returncode != EXIT_SUCCESS:
                errors.append(f"recovery probe exit code {completed.returncode} is not success")
            if envelope.get("pages_materialized") != 1:
                errors.append(f"recovery probe materialized {envelope.get('pages_materialized')!r} pages, expected 1")
            errors.extend(_identity_errors(envelope, candidate_sha, fixture_sha256))
            recovery["recovery_ms"] = wall_ms
            limit = limits.get("recovery_ms")
            if isinstance(limit, int) and wall_ms > limit:
                errors.append(f"recovery_ms {wall_ms} exceeds workload policy {limit}")

    return {
        "fixture_id": fixture_id,
        "workload": workload,
        "fixture_sha256": fixture_sha256,
        "status": "failed" if errors else "measured",
        "cancellation": cancellation,
        "recovery": recovery,
        "validation_errors": errors,
    }


def run_hostile_corpus(
    pdf_tool: Path,
    corpus_dir: Path,
    budgets: Mapping[str, Any],
    timeout_seconds: float,
    rasterizers: int = DEFAULT_RASTERIZERS,
    preflight_profile: Path | None = None,
    runner: Runner = subprocess.run,
) -> dict[str, Any]:
    """Requires every hostile PDF to end in a contained PdfTool disposition.

    Rejecting the input (InputError, ProcessingFailure, budget-exceeded) is a
    correct outcome here; crashing, hanging, or breaching the resident ceiling
    is not.
    """
    pdf_tool = Path(pdf_tool).resolve()
    corpus_dir = Path(corpus_dir).resolve()
    manifest = json.loads((corpus_dir / "manifest.json").read_text(encoding="utf-8"))
    cases = manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError(f"hostile corpus manifest has no cases: {corpus_dir}")
    resident_limit = budgets.get("resource_budget", {}).get("resident_limit_bytes")
    records: list[dict[str, Any]] = []
    for case in cases:
        path = corpus_dir / str(case["pdf"])
        record: dict[str, Any] = {"case_id": case["id"], "path": str(path), "expected": case.get("expected")}
        errors: list[str] = []
        if not path.is_file() or _sha256(path) != case.get("sha256"):
            errors.append("hostile fixture SHA-256 does not match manifest")
        else:
            command = _benchmark_command(pdf_tool, path, rasterizers, preflight_profile)
            record["command"] = command
            completed, failure, wall_ms = _timed_run(runner, command, timeout_seconds)
            record["process_wall_ms"] = wall_ms
            if completed is not None:
                record["process_exit_code"] = completed.returncode
            if failure:
                errors.append(failure)
            elif completed is not None:
                envelope = _envelope_from_process(completed)
                record["disposition"] = envelope.get("status") if envelope else "rejected"
                if envelope is not None:
                    record["result"] = envelope
                    rss = envelope.get("rss_high_water_bytes")
                    if isinstance(rss, int) and isinstance(resident_limit, int) and rss > resident_limit:
                        errors.append(f"RSS {rss} exceeds resident policy {resident_limit}")
        record["validation_errors"] = errors
        record["status"] = "failed" if errors else "contained"
        records.append(record)
    return {
        "corpus": str(corpus_dir),
        "cases": records,
        "summary": {"total": len(records), "contained": sum(record["status"] == "contained" for record in records)},
    }


def _load_fixture_manifest(path: Path) -> dict[str, dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_kind") != "loop-resource-envelope-fixtures" or payload.get("schema_version") != 1:
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
        if "workload" in record and (not isinstance(record["workload"], str) or not record["workload"]):
            raise ValueError(f"fixture manifest workload is invalid: {fixture_id}")
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
    runner: Runner = subprocess.run,
    repetitions: int = 1,
    rasterizers: int = DEFAULT_RASTERIZERS,
    cancel_fixture: str | None = None,
    cancel_after_seconds: float | None = None,
    preflight_profile: Path | None = None,
    hostile_corpus: Path | None = None,
    hostile_timeout_seconds: float | None = None,
    cancel_runner: CancelRunner = _run_benchmark_process,
) -> dict[str, Any]:
    pdf_tool = Path(pdf_tool).resolve()
    baseline_by_fixture = _baseline_records(baseline) if isinstance(baseline, Path) else (baseline or {})
    identity = _candidate_identity()
    candidate_sha = identity["candidate_sha"]
    records: list[dict[str, Any]] = []
    resolved: dict[str, tuple[Path, Mapping[str, Any] | None]] = {}
    for fixture_id, spec in FIXTURE_SPECS.items():
        supplied = fixtures.get(fixture_id)
        if supplied is None:
            record = _empty_result(fixture_id, "fixture-not-supplied" if spec["required"] else "fixture-not-supplied-optional")
            record["required"] = spec["required"]
            records.append(record)
            continue
        metadata = dict(supplied) if isinstance(supplied, Mapping) else None
        fixture_path = (Path(metadata["path"]) if metadata else Path(supplied)).resolve()
        if not fixture_path.is_file():
            record = _empty_result(fixture_id, "fixture-not-found")
            record["required"] = spec["required"]
            records.append(record)
            continue
        resolved[fixture_id] = (fixture_path, metadata)
        record = run_fixture(pdf_tool, fixture_id, fixture_path, budgets, timeout_seconds, baseline_by_fixture.get(fixture_id), margin, runner, metadata, repetitions, rasterizers, bool(metadata), candidate_sha, preflight_profile)
        record["required"] = spec["required"]
        records.append(record)

    probe: dict[str, Any] | None = None
    if cancel_fixture is not None:
        if cancel_fixture not in resolved:
            probe = {"fixture_id": cancel_fixture, "status": "unavailable", "validation_errors": ["cancellation fixture not supplied"]}
        else:
            fixture_path, metadata = resolved[cancel_fixture]
            probe = run_cancellation_probe(pdf_tool, cancel_fixture, fixture_path, budgets, timeout_seconds, cancel_after_seconds or 1.0, candidate_sha,
                                           _fixture_workload(cancel_fixture, metadata), rasterizers, preflight_profile, cancel_runner, runner)

    hostile = None
    if hostile_corpus is not None:
        hostile = run_hostile_corpus(pdf_tool, hostile_corpus, budgets, hostile_timeout_seconds or timeout_seconds, rasterizers, preflight_profile, runner)

    failed = sum(record["status"] == "failed" for record in records)
    flagged = sum(record["required"] and record["status"] in {"flagged", "unavailable"} for record in records)
    skipped = sum(not record["required"] and record["status"] == "unavailable" for record in records)
    return {
        "schema_kind": MATRIX_KIND,
        "schema_version": 3,
        "candidate_sha": identity["candidate_sha"],
        "candidate_identity": identity,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "fixtures": records,
        "cancellation_recovery_probe": probe,
        "hostile": hostile,
        "summary": {
            "total": len(records),
            "measured": sum(record["status"] == "measured" for record in records),
            "flagged": flagged,
            "skipped": skipped,
            "failed": failed,
            "candidate_sha_verified": identity["verified"],
            "cancellation_recovery_probe": probe["status"] if probe else "not-run",
            "hostile_contained": f"{hostile['summary']['contained']}/{hostile['summary']['total']}" if hostile else "not-run",
        },
    }


def matrix_passes(matrix: Mapping[str, Any], strict: bool) -> bool:
    summary = matrix["summary"]
    probe = matrix.get("cancellation_recovery_probe")
    hostile = matrix.get("hostile")
    hostile_failed = bool(hostile) and hostile["summary"]["contained"] != hostile["summary"]["total"]
    if summary["failed"] or hostile_failed or (probe is not None and probe["status"] == "failed"):
        return False
    if not strict:
        return True
    return (
        not summary["flagged"]
        and summary["candidate_sha_verified"]
        and probe is not None and probe["status"] == "measured"
        and hostile is not None
    )


def matrix_failure_reasons(matrix: Mapping[str, Any]) -> list[str]:
    reasons: list[str] = []
    for record in matrix["fixtures"]:
        if record["status"] in {"failed", "flagged"} or (record.get("required") and record["status"] == "unavailable"):
            details = record["validation_errors"] or [record.get("reason", "no detail recorded")]
            reasons.extend(f"fixture {record['fixture_id']} {record['status']}: {error}" for error in details)
            detailed = next((run for run in record.get("runs", []) if run.get("detail")), None)
            if detailed is not None:
                reasons.append(f"fixture {record['fixture_id']} run {detailed['run']} detail: {detailed['detail']}")
    probe = matrix.get("cancellation_recovery_probe")
    if probe is not None and probe["status"] != "measured":
        reasons.extend(f"probe {probe['status']}: {error}" for error in probe["validation_errors"])
    for case in (matrix.get("hostile") or {}).get("cases", []):
        if case["status"] != "contained":
            exit_code = case.get("process_exit_code")
            reasons.extend(f"hostile {case['case_id']} (exit {exit_code}): {error}" for error in case["validation_errors"])
    return reasons


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
    parser.add_argument("--preflight-profile", type=Path, default=DEFAULT_PREFLIGHT_PROFILE,
                        help="profile for the benchmark's measured preflight phase")
    parser.add_argument("--no-preflight", action="store_true", help="omit the preflight phase (records stay incomplete)")
    parser.add_argument("--cancel-fixture", choices=tuple(FIXTURE_SPECS))
    parser.add_argument("--cancel-after-seconds", type=float)
    parser.add_argument("--hostile-corpus", type=Path, help=f"budget-exhaustion corpus directory (strict default: {DEFAULT_HOSTILE_CORPUS.relative_to(ROOT)})")
    parser.add_argument("--hostile-timeout-seconds", type=float, default=60.0)
    parser.add_argument("--strict", action="store_true", help="fail when required fixtures, probes, provenance, or measurements are unavailable")
    args = parser.parse_args(argv)
    try:
        if args.margin <= 0 or args.timeout_seconds <= 0 or args.repetitions < 1 or args.rasterizers < 1 or args.hostile_timeout_seconds <= 0:
            raise ValueError("margin, timeouts, repetitions, and rasterizers must be positive")
        if args.strict and args.manifest is None:
            raise ValueError("--strict requires a fixture --manifest with exact digests and sizes")
        if args.strict and args.cancel_fixture is None:
            raise ValueError("--strict requires --cancel-fixture for the cancellation and recovery probes")
        if args.strict and args.no_preflight:
            raise ValueError("--strict cannot omit the preflight phase")
        if (args.cancel_fixture is None) != (args.cancel_after_seconds is None):
            raise ValueError("--cancel-fixture and --cancel-after-seconds must be supplied together")
        if args.cancel_after_seconds is not None and args.cancel_after_seconds <= 0:
            raise ValueError("cancel-after-seconds must be positive")
        preflight_profile = None if args.no_preflight else args.preflight_profile.resolve()
        if preflight_profile is not None and not preflight_profile.is_file():
            raise ValueError(f"preflight profile not found: {preflight_profile}")
        hostile_corpus = args.hostile_corpus or (DEFAULT_HOSTILE_CORPUS if args.strict else None)
        fixtures: Mapping[str, Path | Mapping[str, Any]] = _load_fixture_manifest(args.manifest) if args.manifest else _fixture_args(args.fixture)
        budgets = json.loads(args.budgets.read_text(encoding="utf-8"))
        matrix = run_matrix(args.pdf_tool, fixtures, budgets, args.timeout_seconds, args.baseline, args.margin,
                            repetitions=args.repetitions, rasterizers=args.rasterizers,
                            cancel_fixture=args.cancel_fixture, cancel_after_seconds=args.cancel_after_seconds,
                            preflight_profile=preflight_profile, hostile_corpus=hostile_corpus,
                            hostile_timeout_seconds=args.hostile_timeout_seconds)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(matrix, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"resource-envelope matrix error: {exc}", file=sys.stderr)
        return 2

    print(json.dumps(matrix["summary"], indent=2))
    for reason in matrix_failure_reasons(matrix):
        print(reason, file=sys.stderr)
    return 0 if matrix_passes(matrix, args.strict) else 1


if __name__ == "__main__":
    raise SystemExit(main())
