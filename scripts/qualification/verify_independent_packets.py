#!/usr/bin/env python3
"""Verify reproduction packet integrity without external validators or imaging libraries."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from scripts.qualification import run_independent_validators as validators

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "docs/independent-claims.json"
REQUIRED_TESTS = {"UnitTestsStandardOracle", "UnitTestsConversionOracle", "UnitTestsIncrementalSave",
                  "UnitTestsOverprintRender", "UnitTestsRepairOperation", "UnitTestsActionList",
                  "UnitTestsPageMasterExport", "UnitTestsGovernedExecution", "UnitTestsPreflightVerdict"}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def member_path(directory: Path, name: str) -> Path:
    path = (directory / name).resolve()
    if not path.is_relative_to(directory.resolve()) or not path.is_file() or Path(name).is_absolute():
        raise ValueError("Invalid reproduction packet member path")
    return path


def seal(directory: Path, lane: dict) -> None:
    lane["members"] = {path.relative_to(directory).as_posix(): digest(path) for path in sorted(directory.rglob("*"))
                       if path.is_file() and path.name != "packet.json"}
    write_json(directory / "packet.json", lane)


def verify(directory: Path, inventory: dict, source_sha: str) -> dict:
    lane = json.loads((directory / "packet.json").read_text(encoding="utf-8"))
    if lane.get("schema") != "loop.independent-qualification-packet" or lane.get("schema_version") != 2:
        raise ValueError("Historical or unrecognized packet cannot qualify")
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha) or lane.get("source_sha") != source_sha:
        raise ValueError("Source SHA mismatch")
    if lane.get("corpus_revision") != inventory["corpus_revision"] or lane.get("inventory_sha256") != digest(INVENTORY):
        raise ValueError("Corpus or inventory identity mismatch")
    members = lane.get("members", {})
    if not members:
        raise ValueError("Empty reproduction packet")
    if any(path.is_symlink() for path in directory.rglob("*")):
        raise ValueError("Reproduction packet cannot contain symbolic links")
    for name in members:
        member_path(directory, name)
    actual = {path.relative_to(directory).as_posix(): digest(path) for path in directory.rglob("*")
              if path.is_file() and path.name != "packet.json"}
    if actual != members:
        raise ValueError("Reproduction packet member integrity failure")
    if lane.get("status") != "passed" or not lane.get("run_url") or not re.fullmatch(r"[0-9a-f]{64}", lane.get("distribution_sha256", "")):
        raise ValueError("Incomplete platform qualification")
    if set(lane.get("tests", {})) != REQUIRED_TESTS or any(item.get("exit_code") != 0 for item in lane["tests"].values()):
        raise ValueError("Missing or failed mandatory native test")
    if set(lane.get("binaries", {})) != REQUIRED_TESTS or any(not re.fullmatch(r"[0-9a-f]{64}", item) for item in lane["binaries"].values()):
        raise ValueError("Native binary identities are unavailable")
    expected = {item["id"]: item["expected"] for item in inventory["fixtures"] + inventory["derived_fixtures"] if item["claim"] != "rendering"}
    if set(lane.get("outcomes", {})) != set(expected):
        raise ValueError("Missing independent fixture coverage")
    for name, status in expected.items():
        outcome = lane["outcomes"][name]
        record = json.loads(member_path(directory, outcome["record"]).read_text(encoding="utf-8"))
        artifact = member_path(directory, outcome["artifact"])
        if record.get("schema") != validators.SCHEMA or record.get("schema_version") != 2 or record.get("candidate_sha") != source_sha or record["input"]["sha256"] != digest(artifact) or record["input"].get("bytes") != artifact.stat().st_size or record.get("platform", {}).get("system") != lane["platform"]:
            raise ValueError("Validator record artifact/source identity mismatch")
        if record.get("status") != status or not record.get("validators"):
            raise ValueError("Expected rejection is a test outcome, never compliant evidence")
        for validator in record["validators"]:
            if validator["status"] != status or not validator.get("version") or not validator.get("command"):
                raise ValueError("Incomplete independent validator record")
            claim = validator["claim"]
            target = {"standards": "pdfa-2b", "signature": "signed-revision-integrity", "structural": "pdf-structure"}.get(claim)
            if not target or validator.get("target") != target or validator.get("scope") != "exact-input-bytes" or not isinstance(validator.get("limitations"), list) or not re.fullmatch(validators.VERSION_PATTERNS[claim], validator["version"], re.IGNORECASE):
                raise ValueError("Unqualified validator target, scope or version")
            raw = member_path(directory, outcome["raw_reports"][claim])
            if digest(raw) != validator["report_sha256"]:
                raise ValueError("Raw report identity mismatch")
            if validator["claim"] == "standards":
                parsed, _ = validators.parse_standard_report(raw.read_bytes(), Path(record["input"]["path"]))
            elif validator["claim"] == "signature":
                parsed, _, _ = validators.parse_signature_report(raw.read_bytes(), artifact.stat().st_size)
            else:
                parsed = "passed" if b"No syntax or stream encoding errors found" in raw.read_bytes() else "incomplete"
            if parsed != status:
                raise ValueError("Recorded verdict disagrees with retained independent report")
    renders = json.loads((directory / "ghostscript/measurements.json").read_text(encoding="utf-8"))
    rendering = {item["id"] for item in inventory["fixtures"] + inventory["derived_fixtures"] if item["claim"] == "rendering"}
    if renders.get("status") != "passed" or {item["fixture"] for item in renders["fixtures"]} != rendering or any(item["status"] != "passed" or not item["planes"] for item in renders["fixtures"]):
        raise ValueError("Independent rendering coverage failed or incomplete")
    fixture_by_id = {item["id"]: item for item in inventory["fixtures"] + inventory["derived_fixtures"]}
    for record in renders["fixtures"]:
        fixture = fixture_by_id[record["fixture"]]
        artifact = member_path(directory, "core-renders/" + record["fixture"] + ".pdf")
        if record.get("artifact_sha256") != digest(artifact) or ("sha256" in fixture and fixture["sha256"] != digest(artifact)):
            raise ValueError("Rendering artifact identity mismatch")
        metadata = json.loads(member_path(directory, "core-renders/" + record["fixture"] + ".json").read_text(encoding="utf-8"))
        if metadata.get("process_space") != "DeviceCMYK" or metadata.get("paper") != "opaque-white" or metadata.get("channel_encoding") != "255*(1-ink*opacity)" or metadata.get("width") != fixture["width"] or metadata.get("height") != fixture["height"]:
            raise ValueError("Unqualified rendering geometry or channel encoding")
        if {plane["separation"] for plane in record["planes"]} != {plane["name"] for plane in metadata["channels"]}:
            raise ValueError("Separation coverage mismatch in reproduction packet")
        for plane in record["planes"]:
            if plane["status"] != "passed" or [region["region"] for region in plane["regions"]] != fixture["regions"] or any(not region["passed"] or region["differing_pixels"] > fixture["differing_pixel_budget"] for region in plane["regions"]):
                raise ValueError("Rendering measurement violates fixture regions or tolerances")
    return lane


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("packets", type=Path, nargs="+")
    args = parser.parse_args()
    inventory = json.loads(INVENTORY.read_text(encoding="utf-8"))
    lanes = [verify(directory, inventory, args.source_sha) for directory in args.packets]
    if len(lanes) != 2 or {lane["platform"] for lane in lanes} != set(inventory["required_platforms"]):
        parser.error("Both Linux and Windows qualification packets are required")
    print("Independent packet integrity passed for both platforms; Core release admission remains with #21.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
