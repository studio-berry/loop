#!/usr/bin/env python3
"""Run the optional external validators used by the 0.2.0 qualification gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
import time
from pathlib import Path
from shutil import which
from typing import Any


SCHEMA = "loop.independent-validation-evidence"
SCHEMA_VERSION = 1
MAX_OUTPUT = 4096
CLAIMS = {
    "structural": ("qpdf", ["--check", "{input}"]),
    "signature": ("pdfsig", ["{input}"]),
    "standards": ("verapdf", ["validate", "--format", "text", "{input}"]),
}


def _short(value: bytes) -> str:
    return value.decode("utf-8", errors="replace")[-MAX_OUTPUT:]


def _version(program: str) -> str | None:
    try:
        completed = subprocess.run([program, "--version"], capture_output=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = (completed.stdout + completed.stderr).decode("utf-8", errors="replace").strip()
    return output[-512:] if output else None


def _input_identity(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            digest.update(chunk)
    return size, digest.hexdigest()


def _run_claim(claim: str, input_path: Path, timeout_ms: int) -> dict[str, Any]:
    program, configured_arguments = CLAIMS[claim]
    executable = which(program)
    result: dict[str, Any] = {
        "claim": claim,
        "program": program,
        "program_path": executable,
        "configured_arguments": configured_arguments,
        "status": "incomplete",
    }
    if not executable:
        result["reason_code"] = "validator-not-installed"
        return result

    result["version"] = _version(executable)
    arguments = [value.replace("{input}", str(input_path)) for value in configured_arguments]
    result["arguments"] = arguments
    started = time.monotonic()
    try:
        completed = subprocess.run(
            [executable, *arguments],
            capture_output=True,
            timeout=max(timeout_ms, 1) / 1000,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        result["duration_ms"] = round((time.monotonic() - started) * 1000)
        result["timed_out"] = True
        result["stdout"] = _short(error.stdout or b"")
        result["stderr"] = _short(error.stderr or b"")
        result["reason_code"] = "validator-timeout"
        return result
    except OSError as error:
        result["duration_ms"] = round((time.monotonic() - started) * 1000)
        result["error"] = str(error)
        result["reason_code"] = "validator-invocation-failed"
        return result

    result["duration_ms"] = round((time.monotonic() - started) * 1000)
    result["exit_code"] = completed.returncode
    result["stdout"] = _short(completed.stdout)
    result["stderr"] = _short(completed.stderr)
    if completed.returncode != 0:
        result["status"] = "rejected"
        result["reason_code"] = "validator-rejected"
        return result

    if claim == "signature":
        validator_output = f"{result['stdout']}\n{result['stderr']}".lower()
        if "no signatures" in validator_output or "no signature" in validator_output:
            result["reason_code"] = "signature-not-present"
            return result

    result["status"] = "passed"
    return result


def run(input_path: Path, claims: list[str], timeout_ms: int, candidate_sha: str | None = None) -> dict[str, Any]:
    size, digest = _input_identity(input_path)
    validators = [_run_claim(claim, input_path, timeout_ms) for claim in claims]
    statuses = {item["status"] for item in validators}
    if "rejected" in statuses:
        status = "rejected"
    elif "incomplete" in statuses:
        status = "incomplete"
    else:
        status = "passed"

    evidence: dict[str, Any] = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "status": status,
        "input": {"path": str(input_path), "bytes": size, "sha256": digest},
        "validators": validators,
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
        },
    }
    if candidate_sha:
        evidence["candidate_sha"] = candidate_sha
    return evidence


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="Candidate PDF to validate")
    parser.add_argument("--output", required=True, type=Path, help="Evidence JSON output path")
    parser.add_argument("--claim", action="append", choices=sorted(CLAIMS), required=True)
    parser.add_argument("--timeout-ms", type=int, default=120000)
    parser.add_argument("--candidate-sha")
    args = parser.parse_args(argv)
    if not args.input.is_file():
        parser.error(f"input PDF does not exist: {args.input}")

    evidence = run(args.input, args.claim, args.timeout_ms, args.candidate_sha)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(evidence, indent=2))
    return {"passed": 0, "rejected": 1, "incomplete": 2}[evidence["status"]]


if __name__ == "__main__":
    sys.exit(main())
