#!/usr/bin/env python3
"""Build Session 10 independent-validation evidence from the frozen fixture manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "docs/evidence/session-10-trust/conversion-fixture-manifest.json"
VALIDATOR = ROOT / "scripts/qualification/run_independent_validators.py"
CLAIMS = ("structural", "signature", "standards")


def _sha256(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            digest.update(chunk)
    return size, digest.hexdigest()


def _git_sha() -> str:
    completed = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def _load_manifest() -> dict:
    with MANIFEST.open(encoding="utf-8") as stream:
        return json.load(stream)


def _resolve_input(manifest: dict) -> Path:
    qualification = manifest["qualification_pdf"]
    path = ROOT / qualification["path"]
    if not path.is_file():
        raise FileNotFoundError(f"qualification PDF missing: {path}")
    size, digest = _sha256(path)
    if digest != qualification["sha256"]:
        raise ValueError(
            f"qualification PDF digest mismatch for {path}: expected {qualification['sha256']}, got {digest} ({size} bytes)"
        )
    return path


def _run_validator(input_path: Path, output_path: Path, candidate_sha: str) -> dict:
    command = [
        sys.executable,
        str(VALIDATOR),
        "--input",
        str(input_path),
        "--output",
        str(output_path),
        "--candidate-sha",
        candidate_sha,
        *sum([["--claim", claim] for claim in CLAIMS], []),
    ]
    completed = subprocess.run(command, cwd=ROOT)
    if completed.returncode not in (0, 1, 2):
        raise subprocess.CalledProcessError(completed.returncode, command)
    with output_path.open(encoding="utf-8") as stream:
        return json.load(stream)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        help="Evidence JSON path (default: docs/evidence/session-10-trust/independent-validation-<platform>.json)",
    )
    parser.add_argument("--candidate-sha", help="40-char candidate SHA (default: git HEAD)")
    args = parser.parse_args()

    manifest = _load_manifest()
    input_path = _resolve_input(manifest)
    candidate_sha = args.candidate_sha or _git_sha()

    system = platform.system().lower()
    default_name = "windows" if system == "windows" else "linux"
    output_path = args.output or ROOT / f"docs/evidence/session-10-trust/independent-validation-{default_name}.json"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    evidence = _run_validator(input_path, output_path, candidate_sha)
    print(json.dumps(evidence, indent=2))
    return {"passed": 0, "rejected": 1, "incomplete": 2}[evidence["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
