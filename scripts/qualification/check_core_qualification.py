#!/usr/bin/env python3
"""Check L01 CI and package provenance without granting module admission."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Sequence


ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.ci.compare_package_boundary_evidence import FULL_SHA, compare


BUILD_STEPS = (
    "Build Widgets-absent release profile",
    "Test Widgets-absent release profile",
    "Build project",
    "Run unit tests",
    "Run preflight corpus gate",
)
REQUIRED_JOBS = {
    "source_integrity": ("Verify tracked source integrity",),
    "linux / build": BUILD_STEPS,
    "windows / build": BUILD_STEPS,
}


def load_run(path: Path) -> dict[str, Any]:
    snapshot = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(snapshot, dict):
        raise ValueError("CI snapshot must be a JSON object")
    return snapshot


def require_success(record: dict[str, Any], label: str) -> None:
    if record.get("status") != "completed" or record.get("conclusion") != "success":
        raise ValueError(f"{label} must be completed and successful")


def unique_records(records: Any, required: Sequence[str], label: str) -> dict[str, dict[str, Any]]:
    if not isinstance(records, list) or any(not isinstance(item, dict) for item in records):
        raise ValueError(f"{label} must be an array of objects")
    selected = {}
    for name in required:
        matches = [item for item in records if item.get("name") == name]
        if len(matches) != 1:
            raise ValueError(f"{label}: expected exactly one {name!r}; found {len(matches)}")
        selected[name] = matches[0]
    return selected


def validate_run(snapshot: dict[str, Any], candidate_sha: str) -> dict[str, dict[str, Any]]:
    if snapshot.get("headSha") != candidate_sha:
        raise ValueError("CI headSha must equal candidate SHA")
    run_id = snapshot.get("databaseId")
    if type(run_id) is not int or run_id <= 0:
        raise ValueError("CI databaseId must be a positive integer")
    if snapshot.get("url") not in (
        f"https://github.com/studio-berry/loop/actions/runs/{run_id}",
        f"https://github.com/studio-berry/loop2/actions/runs/{run_id}",
    ):
        raise ValueError("CI URL must identify this repository and run ID")
    require_success(snapshot, "CI run")
    jobs = unique_records(snapshot.get("jobs"), tuple(REQUIRED_JOBS), "CI jobs")
    job_ids = set()
    for name, job in jobs.items():
        require_success(job, name)
        job_id = job.get("databaseId")
        if type(job_id) is not int or job_id <= 0 or job_id in job_ids:
            raise ValueError(f"{name}: expected a unique positive job ID")
        job_ids.add(job_id)
        steps = unique_records(job.get("steps"), REQUIRED_JOBS[name], f"{name} steps")
        for step_name, step in steps.items():
            require_success(step, f"{name}: {step_name}")
    return jobs


def verify_package(path: Path, identity: dict[str, Any]) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"package must be a regular file: {path.name}")
    if identity.get("name") != path.name:
        raise ValueError(f"package name differs from evidence: {path.name}")
    expected_size = identity.get("size")
    if type(expected_size) is not int or expected_size <= 0:
        raise ValueError(f"package evidence needs a positive byte count: {path.name}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            digest.update(chunk)
    if size != expected_size:
        raise ValueError(f"package byte count differs from evidence: {path.name}")
    if digest.hexdigest() != identity["sha256"].lower():
        raise ValueError(f"package SHA-256 differs from evidence: {path.name}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--ci-run", type=Path, required=True, help="gh run view JSON snapshot")
    parser.add_argument("--linux-evidence", type=Path, required=True)
    parser.add_argument("--windows-evidence", type=Path, required=True)
    parser.add_argument("--linux-package", type=Path, required=True)
    parser.add_argument("--windows-package", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if not FULL_SHA.fullmatch(args.candidate_sha):
            raise ValueError("candidate SHA must be a full 40-character Git SHA")
        candidate_sha = args.candidate_sha.lower()
        snapshot = load_run(args.ci_run)
        jobs = validate_run(snapshot, candidate_sha)
        pair = compare(args.linux_evidence, args.windows_evidence, candidate_sha)
        verify_package(args.linux_package, pair["packages"]["linux"])
        verify_package(args.windows_package, pair["packages"]["windows"])
    except (OSError, ValueError) as error:
        print(f"Core qualification provenance rejected: {error}", file=sys.stderr)
        return 1

    print(f"Core qualification provenance verified for {candidate_sha}")
    print(f"CI run: {snapshot['url']}")
    for name, job in jobs.items():
        print(f"{name}: {snapshot['url']}/job/{job['databaseId']}")
    for platform, package in pair["packages"].items():
        print(f"{platform}: {package['name']} bytes={package['size']} sha256={package['sha256']}")
    print("Module admission: PENDING REVIEW. Inspect test details, all L01 child evidence, and reviewer decision.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
