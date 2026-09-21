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
BUNDLE_SCHEMA = "loop.preflight-evidence-bundle"
BUNDLE_SCHEMA_VERSION = 1
BUNDLE_MANIFEST = "manifest.json"
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


def run(input_path: Path, claims: list[str], timeout_ms: int, candidate_sha: str | None = None,
        bundle_path: Path | None = None) -> dict[str, Any]:
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
    if bundle_path is not None:
        evidence["bundle"] = verify_bundle(bundle_path, digest)
        # A bundle that does not verify cannot be consumed, so it fails the lane
        # closed the same way an incomplete or rejected validator does.
        if evidence["bundle"]["status"] == "rejected":
            evidence["status"] = "rejected"
        elif evidence["bundle"]["status"] == "incomplete" and evidence["status"] == "passed":
            evidence["status"] = "incomplete"
    if candidate_sha:
        evidence["candidate_sha"] = candidate_sha
    return evidence


def verify_bundle(bundle_path: Path, input_digest: str) -> dict[str, Any]:
    """Consume a portable proof-of-preflight bundle without Loop.

    The bundle is verified from its own bytes: every declared member is hashed
    and compared with the manifest, the directory may carry nothing the manifest
    does not declare, and the manifest's document revision digest must be the
    digest of the candidate the validators are about to run on. This is the
    third-party reading of `docs/PREFLIGHT_EVIDENCE_BUNDLE.md`; Loop's own
    `PdfTool verify-evidence-bundle` checks the identity chain inside the
    bundle, which this consumer does not re-implement.
    """
    result: dict[str, Any] = {
        "path": str(bundle_path),
        "status": "incomplete",
        "members_verified": 0,
        "declared_members": [],
        "missing_members": [],
        "undeclared_members": [],
        "input_matches_manifest": False,
    }
    manifest_path = bundle_path / BUNDLE_MANIFEST
    if not bundle_path.is_dir():
        result["reason_code"] = "bundle-not-found"
        return result
    if not manifest_path.is_file():
        result["reason_code"] = "manifest-not-found"
        return result

    manifest_bytes = manifest_path.read_bytes()
    result["manifest_sha256"] = hashlib.sha256(manifest_bytes).hexdigest()
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        result["status"] = "rejected"
        result["reason_code"] = "manifest-unreadable"
        return result
    if not isinstance(manifest, dict):
        result["status"] = "rejected"
        result["reason_code"] = "manifest-unreadable"
        return result

    result["manifest_schema"] = manifest.get("schema")
    if manifest.get("schema") != BUNDLE_SCHEMA or manifest.get("schema_version") != BUNDLE_SCHEMA_VERSION:
        result["status"] = "rejected"
        result["reason_code"] = "bundle-schema-unsupported"
        return result

    members = manifest.get("members")
    if not isinstance(members, list) or not members:
        result["status"] = "rejected"
        result["reason_code"] = "manifest-members-missing"
        return result

    declared: set[str] = set()
    mismatched: list[str] = []
    for entry in members:
        if not isinstance(entry, dict):
            result["status"] = "rejected"
            result["reason_code"] = "manifest-member-invalid"
            return result
        name = str(entry.get("name", ""))
        declared.add(name)
        result["declared_members"].append(name)
        member_path = bundle_path / name
        if not member_path.is_file():
            result["missing_members"].append(name)
            continue
        content = member_path.read_bytes()
        if hashlib.sha256(content).hexdigest() != str(entry.get("sha256", "")).lower():
            mismatched.append(name)
            continue
        if entry.get("byte_count") is not None and int(entry["byte_count"]) != len(content):
            mismatched.append(name)
            continue
        result["members_verified"] += 1

    for present in sorted(path.name for path in bundle_path.iterdir() if path.is_file()):
        if present != BUNDLE_MANIFEST and present not in declared:
            result["undeclared_members"].append(present)

    document = manifest.get("document")
    declared_digest = document.get("revision_digest") if isinstance(document, dict) else None
    result["document_revision_digest"] = declared_digest
    result["input_matches_manifest"] = bool(declared_digest) and str(declared_digest).lower() == input_digest.lower()

    if result["missing_members"]:
        result["status"] = "rejected"
        result["reason_code"] = "member-missing"
    elif mismatched:
        result["status"] = "rejected"
        result["reason_code"] = "member-digest-mismatch"
        result["mismatched_members"] = mismatched
    elif result["undeclared_members"]:
        result["status"] = "rejected"
        result["reason_code"] = "member-undeclared"
    elif not result["input_matches_manifest"]:
        result["status"] = "rejected"
        result["reason_code"] = "input-does-not-match-manifest"
    else:
        result["status"] = "passed"
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="Candidate PDF to validate")
    parser.add_argument("--output", required=True, type=Path, help="Evidence JSON output path")
    parser.add_argument("--claim", action="append", choices=sorted(CLAIMS), required=True)
    parser.add_argument("--timeout-ms", type=int, default=120000)
    parser.add_argument("--candidate-sha")
    parser.add_argument(
        "--evidence-bundle",
        type=Path,
        help="Portable proof-of-preflight bundle to consume; the candidate must be its declared revision",
    )
    args = parser.parse_args(argv)
    if not args.input.is_file():
        parser.error(f"input PDF does not exist: {args.input}")

    evidence = run(args.input, args.claim, args.timeout_ms, args.candidate_sha, args.evidence_bundle)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(evidence, indent=2))
    return {"passed": 0, "rejected": 1, "incomplete": 2}[evidence["status"]]


if __name__ == "__main__":
    sys.exit(main())
