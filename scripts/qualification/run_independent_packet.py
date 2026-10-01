#!/usr/bin/env python3
"""Produce and verify source-bound Windows/Linux independent qualification packets."""
from __future__ import annotations

import argparse
import base64
import json
import os
import platform
import re
import shutil
import subprocess
from pathlib import Path

from scripts.qualification import run_independent_validators as validators
from scripts.qualification.compare_independent_renders import compare
from scripts.qualification.verify_independent_packets import REQUIRED_TESTS, digest, seal, verify, write_json

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "docs/independent-claims.json"


def run(build: Path, output: Path, distribution: Path, distribution_sha256: str, source_sha: str, run_url: str) -> int:
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha) or not re.fullmatch(r"[0-9a-f]{64}", distribution_sha256):
        raise ValueError("Full source and distribution identities are required")
    current = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    if current != source_sha or subprocess.run(["git", "diff", "--quiet", "HEAD", "--"], cwd=ROOT).returncode != 0:
        raise ValueError("Qualification requires an unchanged checkout of the specified source SHA")
    archive = distribution.with_suffix(".zip")
    if not archive.is_file() or digest(archive) != distribution_sha256:
        raise ValueError("Pinned oracle archive identity is unavailable")
    if output.exists():
        raise ValueError("Qualification output must be a new directory")
    output.mkdir(parents=True)
    inventory = json.loads(INVENTORY.read_text(encoding="utf-8"))
    lock = json.loads((distribution / "oracles.json").read_text(encoding="utf-8"))
    tools = {name: (distribution / item["path"]).resolve() for name, item in lock["tools"].items()}
    for name, path in tools.items():
        if digest(path) != lock["tools"][name]["sha256"]:
            raise ValueError("Oracle binary identity changed after installation")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen",
           "LOOP_INDEPENDENT_RENDER_DIR": str(output / "core-renders"),
           "LOOP_INDEPENDENT_CONVERSION_DIR": str(output / "conversion"),
           "LOOP_SAVE_POLICY_EVIDENCE_DIR": str(output / "save-policy")}
    env["PATH"] = os.pathsep.join(sorted({str(path.parent) for path in tools.values()})) + os.pathsep + env.get("PATH", "")
    os.environ["PATH"] = env["PATH"]
    for claim, (program, _) in validators.CLAIMS.items():
        expected = tools[{"structural": "qpdf", "signature": "pdfsig", "standards": "verapdf"}[claim]]
        found = shutil.which(program)
        if not found or Path(found).resolve() != expected:
            raise ValueError("PATH resolved a different oracle than the pinned distribution")
    listing = subprocess.run(["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"], capture_output=True, check=True)
    tests = {item["name"]: item["command"] for item in json.loads(listing.stdout)["tests"] if item["name"] in REQUIRED_TESTS}
    if set(tests) != REQUIRED_TESTS:
        raise ValueError(f"Required native targets absent: {REQUIRED_TESTS - set(tests)}")
    shutil.copy2(build / "CMakeCache.txt", output / "CMakeCache.txt")
    shutil.copy2(distribution / "oracles.json", output / "oracles.json")
    lane = {"schema": "loop.independent-qualification-packet", "schema_version": 2, "source_sha": source_sha,
            "corpus_revision": inventory["corpus_revision"], "inventory_sha256": digest(INVENTORY),
            "distribution_sha256": distribution_sha256, "platform": platform.system(), "run_url": run_url,
            "tests": {}, "binaries": {}, "outcomes": {}, "status": "incomplete"}
    failures = []
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache, re.MULTILINE)
    if not home or Path(home.group(1)).resolve() != ROOT:
        raise ValueError("Build directory belongs to another source checkout")
    for name, command in sorted(tests.items()):
        lane["binaries"][name] = digest(Path(command[0]))
        with (output / (name + ".log")).open("wb") as report:
            completed = subprocess.run(command, env=env, cwd=build, stdout=report, stderr=subprocess.STDOUT, timeout=600, check=False)
        lane["tests"][name] = {"command": command, "exit_code": completed.returncode}
        if completed.returncode != 0:
            failures.append(name)
    for fixture in inventory["fixtures"] + inventory["derived_fixtures"]:
        if fixture["claim"] == "rendering":
            continue
        name = fixture["id"]
        source = ROOT / fixture["path"] if "path" in fixture else output / fixture["artifact"]
        if not source.is_file():
            failures.append("missing artifact " + name)
            continue
        artifact = output / (name + ".pdf")
        shutil.copy2(source, artifact)
        if "sha256" in fixture and digest(artifact) != fixture["sha256"]:
            raise ValueError("Pinned fixture identity changed")
        claims = [fixture["claim"]]
        record = validators.run(artifact, claims, 120000, source_sha)
        reports = {}
        for item in record["validators"]:
            raw = name + "." + item["claim"] + ".stdout"
            (output / raw).write_bytes(base64.b64decode(item.pop("stdout_base64", ""), validate=True))
            (output / (name + "." + item["claim"] + ".stderr")).write_bytes(item.get("stderr", "").encode("utf-8"))
            reports[item["claim"]] = raw
        record_path = name + ".evidence.json"
        write_json(output / record_path, record)
        lane["outcomes"][name] = {"expected": fixture["expected"], "observed": record["status"], "artifact": artifact.name,
                                  "record": record_path, "raw_reports": reports}
        if record["status"] != fixture["expected"]:
            failures.append(name)
    if not failures:
        renders = compare(ROOT, inventory, output / "core-renders", output / "ghostscript", distribution)
        write_json(output / "ghostscript/measurements.json", renders)
        if renders["status"] != "passed":
            failures.append("independent rendering mismatch")
    lane["failures"] = failures
    lane["status"] = "rejected" if failures else "passed"
    for fixture in inventory["fixtures"]:
        if digest(ROOT / fixture["path"]) != fixture["sha256"]:
            lane["failures"].append("source fixture mutated: " + fixture["id"])
            lane["status"] = "rejected"
    seal(output, lane)
    if lane["status"] == "passed":
        verify(output, inventory, source_sha)
    return 0 if lane["status"] == "passed" else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--distribution", type=Path)
    parser.add_argument("--distribution-sha256")
    parser.add_argument("--run-url")
    parser.add_argument("--verify", type=Path, nargs="+")
    args = parser.parse_args()
    inventory = json.loads(INVENTORY.read_text(encoding="utf-8"))
    if args.verify:
        lanes = [verify(directory, inventory, args.source_sha) for directory in args.verify]
        if len(lanes) != 2 or {lane["platform"] for lane in lanes} != set(inventory["required_platforms"]):
            parser.error("Both Linux and Windows qualification packets are required")
        print("Independent qualification packet integrity passed for both platforms; Core release admission remains with #21.")
        return 0
    if not all((args.build, args.output, args.distribution, args.distribution_sha256, args.run_url)):
        parser.error("Run mode requires build, output, distribution, distribution SHA-256 and run URL")
    return run(args.build.resolve(), args.output.resolve(), args.distribution.resolve(), args.distribution_sha256, args.source_sha, args.run_url)


if __name__ == "__main__":
    raise SystemExit(main())
