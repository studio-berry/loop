#!/usr/bin/env python3
"""Run the optional external validators used by the 0.2.0 qualification gate."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import platform
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path, PureWindowsPath
from shutil import which
from typing import Any


SCHEMA = "loop.independent-validation-evidence"
SCHEMA_VERSION = 2
MAX_OUTPUT = 16 * 1024 * 1024
BUNDLE_SCHEMA = "loop.preflight-evidence-bundle"
BUNDLE_SCHEMA_VERSION = 1
BUNDLE_MANIFEST = "manifest.json"
CLAIMS = {
    "structural": ("qpdf", ["--check", "{input}"]),
    "signature": ("pdfsig", ["{input}"]),
    "standards": ("verapdf", ["--format", "xml", "--flavour", "2b", "{input}"]),
}


VERSION_PATTERNS = {
    "structural": r"qpdf(?:\.exe)? version (?:11|12)\.\d+(?:\.\d+)?",
    "signature": r"pdfsig version (?:2[3-9])\.\d+(?:\.\d+)?",
    "standards": r"veraPDF(?: CLI)? (?:version )?1\.(?:24|26|28|30)\.\d+",
}


def _short(value: bytes) -> str:
    return value.decode("utf-8", errors="replace")[-MAX_OUTPUT:]


def _version(program: str, claim: str) -> str | None:
    flag = "-v" if claim == "signature" else "--version"
    try:
        completed = subprocess.run([program, flag], capture_output=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = (completed.stdout + completed.stderr).decode("utf-8", errors="replace").strip()
    match = re.search(VERSION_PATTERNS[claim], output, re.IGNORECASE)
    return match.group(0) if completed.returncode == 0 and match else None


def parse_standard_report(raw: bytes, input_path: Path) -> tuple[str, str]:
    try:
        root = ET.fromstring(raw)
    except ET.ParseError:
        return "incomplete", "validator-report-invalid"
    jobs = root.findall("./jobs/job")
    reports = root.findall("./jobs/job/validationReport")
    if root.tag != "report" or len(jobs) != 1 or len(reports) != 1:
        return "incomplete", "validator-report-incomplete"
    item = jobs[0].find("item")
    reported_name = item.findtext("name", "") if item is not None else ""
    windows_input = PureWindowsPath(str(input_path))
    if windows_input.is_absolute():
        reported_path = PureWindowsPath(reported_name)
        matches_input = reported_path.is_absolute() and reported_path == windows_input
    else:
        reported_path = Path(reported_name)
        matches_input = reported_path.is_absolute() and reported_path.resolve() == input_path.resolve()
    if not matches_input:
        return "incomplete", "validator-input-mismatch"
    if any(element.tag.endswith("Exception") for element in root.iter()):
        return "incomplete", "validator-report-incomplete"
    report = reports[0]
    if report.get("jobEndStatus", "normal") != "normal":
        return "incomplete", "validator-report-incomplete"
    if report.get("profileName", "").upper() != "PDF/A-2B VALIDATION PROFILE":
        return "incomplete", "validator-target-mismatch"
    if report.get("isCompliant") == "false":
        return "rejected", "validator-rejected"
    details = report.findall("details")
    summary = root.findall("batchSummary")
    if report.get("isCompliant") != "true" or len(details) != 1 or len(summary) != 1:
        return "incomplete", "validator-report-incomplete"
    counts = details[0].attrib
    if any(not re.fullmatch(r"[0-9]+", counts.get(key, "")) for key in ("passedRules", "failedRules", "passedChecks", "failedChecks")):
        return "incomplete", "validator-report-incomplete"
    if int(counts["passedRules"]) == 0 or int(counts["passedChecks"]) == 0 or int(counts["failedRules"]) != 0 or int(counts["failedChecks"]) != 0:
        return "incomplete", "validator-report-inconsistent"
    if any(summary[0].get(key) != value for key, value in {"totalJobs": "1", "failedToParse": "0", "encrypted": "0", "outOfMemory": "0", "veraExceptions": "0"}.items()):
        return "incomplete", "validator-report-incomplete"
    totals = summary[0].findall("validationReports")
    if len(totals) != 1 or totals[0].get("compliant") != "1" or totals[0].get("nonCompliant") != "0" or totals[0].get("failedJobs") != "0" or totals[0].text != "1":
        return "incomplete", "validator-report-inconsistent"
    return "passed", ""


def parse_signature_report(raw: bytes, input_bytes: int | None = None) -> tuple[str, str, list[dict]]:
    output = raw.decode("utf-8", errors="replace")
    numbers = re.findall(r"(?m)^Signature #(\d+):\s*$", output)
    blocks = re.split(r"(?m)^Signature #\d+:\s*$", output)[1:]
    if numbers and numbers != [str(index) for index in range(1, len(numbers) + 1)]:
        return "incomplete", "signature-report-incomplete", []
    if not blocks:
        return "incomplete", "signature-not-present" if "no signatures" in output.lower() else "validator-report-invalid", []
    signatures = []
    for block in blocks:
        validation = re.findall(r"Signature Validation: ([^\r\n]+)", block)
        ranges = re.findall(r"Signed Ranges: ([^\r\n]+)", block)
        coverage = re.findall(r"(Total document signed|Not total document signed)", block)
        trust = re.findall(r"Certificate Validation: ([^\r\n]+)", block)
        signatures.append({"integrity": validation[0].strip() if len(validation) == 1 else "unavailable",
                           "signed_ranges": ranges[0].strip() if len(ranges) == 1 else "unavailable",
                           "coverage": coverage[0] if len(coverage) == 1 else "unavailable",
                           "certificate_trust": trust[0].strip() if len(trust) == 1 else "not-evaluated"})
    for record in signatures:
        raw_ranges = record["signed_ranges"]
        pairs = re.findall(r"\[([0-9]+) - ([0-9]+)\]", raw_ranges)
        complete = re.fullmatch(r"\[[0-9]+ - [0-9]+\](?:, \[[0-9]+ - [0-9]+\])+", raw_ranges)
        intervals = [(int(start), int(end)) for start, end in pairs]
        valid = bool(complete) and intervals[0][0] == 0 and all(start < end for start, end in intervals) and all(first[1] < second[0] for first, second in zip(intervals, intervals[1:]))
        if valid and input_bytes is not None:
            valid = intervals[-1][1] <= input_bytes and ((record["coverage"] == "Total document signed") == (intervals[-1][1] == input_bytes))
        record["signed_revision_end"] = intervals[-1][1] if valid else None
    rejected = {"Signature is Invalid.", "Digest Mismatch.", "Document isn't signed or corrupted data."}
    if any(record["integrity"] in rejected for record in signatures):
        return "rejected", "signature-invalid", signatures
    if any(record["integrity"] != "Signature is Valid." or record["signed_revision_end"] is None or record["coverage"] == "unavailable" for record in signatures):
        return "incomplete", "signature-report-incomplete", signatures
    return "passed", "", signatures


def _input_identity(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            digest.update(chunk)
    return size, digest.hexdigest()


def _run_claim(claim: str, input_path: Path, timeout_ms: int, input_bytes: int | None = None) -> dict[str, Any]:
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

    try:
        result["version"] = _version(executable, claim)
    except KeyboardInterrupt:
        result["reason_code"] = "validator-cancelled"
        return result
    if result["version"] is None:
        result["reason_code"] = "validator-version-unsupported"
        return result
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
    except KeyboardInterrupt:
        result["reason_code"] = "validator-cancelled"
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
    result["report_sha256"] = hashlib.sha256(completed.stdout).hexdigest()
    result["stderr_sha256"] = hashlib.sha256(completed.stderr).hexdigest()
    if len(completed.stdout) > MAX_OUTPUT or len(completed.stderr) > MAX_OUTPUT:
        result["reason_code"] = "validator-report-too-large"
        return result
    result["stdout_base64"] = base64.b64encode(completed.stdout).decode("ascii")
    if claim == "standards":
        result["status"], reason = parse_standard_report(completed.stdout, input_path)
    elif claim == "signature":
        result["status"], reason, result["signatures"] = parse_signature_report(completed.stdout, input_bytes)
    elif completed.returncode == 2:
        result["status"], reason = "rejected", "validator-rejected"
    elif completed.returncode == 0 and b"No syntax or stream encoding errors found" in completed.stdout:
        result["status"], reason = "passed", ""
    else:
        result["status"], reason = "incomplete", "validator-report-incomplete"
    if completed.returncode != 0 and result["status"] == "passed":
        result["status"], reason = "incomplete", "validator-invocation-failed"
    if reason:
        result["reason_code"] = reason
    return result


def run(input_path: Path, claims: list[str], timeout_ms: int, candidate_sha: str | None = None,
        bundle_path: Path | None = None) -> dict[str, Any]:
    if not claims or any(claim not in CLAIMS for claim in claims):
        raise ValueError("At least one recognized claim is required")
    if not candidate_sha or not re.fullmatch(r"[0-9a-f]{40}", candidate_sha):
        raise ValueError("Evidence v2 requires the full 40-character source SHA")
    input_path = input_path.resolve()
    size, digest = _input_identity(input_path)
    validators = [_run_claim(claim, input_path, timeout_ms, size) for claim in claims]
    try:
        unchanged = (size, digest) == _input_identity(input_path)
    except OSError:
        unchanged = False
    for item in validators:
        item.setdefault("version", None)
        item.setdefault("report_sha256", None)
        item["command"] = ([item["program_path"], *item.get("arguments", [])] if item["program_path"] else [])
        item["target"] = {"structural": "pdf-structure", "signature": "signed-revision-integrity", "standards": "pdfa-2b"}[item["claim"]]
        item["scope"] = "exact-input-bytes"
        item["limitations"] = (["Certificate trust and unsigned revisions are not established by signature integrity."]
                                if item["claim"] == "signature" else [])
        if not unchanged:
            item["status"] = "rejected"
            item["reason_code"] = "validator-input-mutated"
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
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument(
        "--evidence-bundle",
        type=Path,
        help="Portable proof-of-preflight bundle to consume; the candidate must be its declared revision",
    )
    args = parser.parse_args(argv)
    if not args.input.is_file():
        parser.error(f"input PDF does not exist: {args.input}")

    try:
        evidence = run(args.input, args.claim, args.timeout_ms, args.candidate_sha, args.evidence_bundle)
    except ValueError as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(evidence, indent=2))
    return {"passed": 0, "rejected": 1, "incomplete": 2}[evidence["status"]]


if __name__ == "__main__":
    sys.exit(main())
