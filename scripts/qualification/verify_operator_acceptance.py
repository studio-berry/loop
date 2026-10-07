#!/usr/bin/env python3
"""Check retained L03-06 operator packets; human review owns admission."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath

from scripts.ci import check_governed_parity as governed
from scripts.ci import compare_package_boundary_evidence as packages
from scripts.qualification.check_core_qualification import verify_package


STEPS = ("open", "inspect", "locate", "preview", "approve", "publish", "revalidate", "sign-off")
ATTESTATIONS = ("installed_product", "real_core", "representative_pdf", "complete_journey")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def read_object(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{path.name} must contain an object")
    return value


def member(directory: Path, name: str) -> Path:
    if not isinstance(name, str) or not name or Path(name).is_absolute():
        raise ValueError("packet member must be a relative path")
    path = (directory / name).resolve()
    if not path.is_relative_to(directory.resolve()) or not path.is_file():
        raise ValueError(f"packet member is missing or escapes the packet: {name}")
    return path


def require_text(record: dict, fields: tuple[str, ...], label: str) -> None:
    if any(not isinstance(record.get(key), str) or not record[key].strip() for key in fields):
        raise ValueError(f"{label} requires {', '.join(fields)}")


def verify(directory: Path, source_sha: str) -> tuple[str, tuple[str, ...]]:
    directory = directory.resolve()
    if any(path.is_symlink() for path in directory.rglob("*")):
        raise ValueError("packet cannot contain symbolic links")
    packet_path = directory / "packet.json"
    packet = read_object(packet_path)
    if (packet.get("kind") != "loop-operator-acceptance-packet"
            or type(packet.get("version")) is not int or packet["version"] != 1):
        raise ValueError("unsupported operator packet")
    platform = packet.get("platform")
    if platform not in ("linux", "windows") or packet.get("source_sha") != source_sha:
        raise ValueError("packet platform or source SHA mismatch")
    if packet.get("status") != "passed" or packet.get("unavailable") != []:
        raise ValueError("operator journey is failed, pending or has unavailable lanes")
    members = packet.get("members")
    if not isinstance(members, dict) or not members:
        raise ValueError("packet members are missing")
    for name, expected in members.items():
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError(f"invalid member digest: {name}")
        if digest(member(directory, name)) != expected:
            raise ValueError(f"member digest mismatch: {name}")
    actual = {path.relative_to(directory).as_posix() for path in directory.rglob("*")
              if path.is_file() and path != packet_path and path != directory / "review.json"}
    if actual != set(members):
        raise ValueError("packet has unbound or missing members")

    def artifact(role: str) -> Path:
        name = packet.get(role)
        if not isinstance(name, str) or name not in members:
            raise ValueError(f"missing bound {role}")
        path = member(directory, name)
        if path.stat().st_size == 0:
            raise ValueError(f"empty {role}")
        return path

    boundary = packages.load(artifact("package_evidence"), platform)
    if boundary["source_sha"] != source_sha:
        raise ValueError("package evidence belongs to a different source SHA")
    verify_package(artifact("package"), boundary["package"])
    environment = read_object(artifact("environment"))
    require_text(environment, ("os", "os_version", "architecture", "host", "recorded_utc"), "environment")
    if environment.get("platform") != platform or environment.get("clean_machine") is not True:
        raise ValueError("clean-machine platform metadata is missing")
    installation = read_object(artifact("installation"))
    require_text(installation, ("command", "installed_root"), "installation")
    if (installation.get("status") != "passed" or installation.get("source_sha") != source_sha
            or installation.get("package_sha256") != boundary["package"]["sha256"]
            or type(installation.get("exit_code")) is not int or installation["exit_code"] != 0):
        raise ValueError("installation does not bind the inspected package")
    launch = read_object(artifact("launch"))
    require_text(launch, ("command", "executable", "installed_root"), "launch")
    if (launch.get("surface") != "LoopEditor" or launch.get("scope") != "installed-product"
            or launch.get("status") != "passed" or launch.get("source_sha") != source_sha
            or launch["installed_root"] != installation["installed_root"]
            or launch.get("package_sha256") != installation["package_sha256"]):
        raise ValueError("journey was not captured against the installed product")
    payload_record = boundary.get("payload")
    if not isinstance(payload_record, dict) or not isinstance(payload_record.get("files"), list):
        raise ValueError("package payload inventory is missing")
    payload = payload_record["files"]
    path_type = PureWindowsPath if platform == "windows" else PurePosixPath
    executable = path_type(launch["executable"])
    installed_root = path_type(launch["installed_root"])
    expected_name = "LoopEditor.exe" if platform == "windows" else "LoopEditor"
    executable_digest = launch.get("executable_sha256")
    if (not installed_root.is_absolute() or not executable.is_relative_to(installed_root)
            or ".." in executable.parts or executable.name != expected_name
            or not isinstance(executable_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", executable_digest)
            or not isinstance(launch.get("payload_path"), str)
            or path_type(launch["payload_path"]).name != expected_name):
        raise ValueError("launch must identify the installed LoopEditor executable and digest")
    if not any(isinstance(row, dict) and row.get("path") == launch.get("payload_path")
               and row.get("sha256") == launch.get("executable_sha256") for row in payload):
        raise ValueError("launched executable is not bound to the inspected package payload")

    artifact("video")
    artifact("log")
    source_digest = digest(artifact("source_pdf"))
    published_digest = digest(artifact("published_pdf"))
    inspection = read_object(artifact("inspection"))
    if inspection.get("document_revision_digest") != source_digest or inspection.get("inspection_complete") is not True:
        raise ValueError("initial inspection does not bind the source PDF or is incomplete")
    if any(not isinstance(inspection.get(key), list) for key in ("errors", "warnings")):
        raise ValueError("inspection findings must be arrays")
    findings = inspection["errors"] + inspection["warnings"]
    finding_ids = {entry.get("id") for entry in findings if isinstance(entry, dict)}
    receipts = list(governed.records(read_object(artifact("receipt")), "receipt"))
    if len(receipts) != 1:
        raise ValueError("exactly one governed publication receipt is required")
    location, receipt = receipts[0]
    errors, identity = governed.validate_governed(receipt, location, False)
    if errors:
        raise ValueError("; ".join(errors))
    sign_off = receipt["sign_off"]
    if sign_off["source_sha256"] != source_digest or sign_off["published_sha256"] != published_digest:
        raise ValueError("receipt does not sign off the retained source and published PDF bytes")
    if sign_off["effective_profile_digest"] != inspection.get("effective_profile_digest"):
        raise ValueError("inspection and publication profile identities differ")
    steps = packet.get("steps")
    if (not isinstance(steps, list) or any(not isinstance(step, dict) for step in steps)
            or [step.get("action") for step in steps] != list(STEPS)):
        raise ValueError("operator steps are missing, duplicated or out of order")
    for step in steps:
        if step.get("status") != "passed" or step.get("implementation") != "product-core":
            raise ValueError(f"step is skipped, failed or mocked: {step['action']}")
        offset = step.get("video_seconds")
        if type(offset) not in (int, float) or not math.isfinite(offset) or offset < 0:
            raise ValueError("step requires a nonnegative video offset")
        require_text(step, ("observation",), step["action"])
    if any(before["video_seconds"] > after["video_seconds"] for before, after in zip(steps, steps[1:])):
        raise ValueError("step video offsets must follow journey order")
    if steps[2].get("finding_id") not in finding_ids or not steps[2].get("finding_id"):
        raise ValueError("located finding is absent from the real inspection")
    for step in steps[3:]:
        if step.get("plan_digest") != sign_off["plan_digest"]:
            raise ValueError("preview through sign-off must bind the same plan")

    review = read_object(directory / "review.json")
    require_text(review, ("reviewer", "reviewed_utc", "rationale"), "human review")
    attestations = review.get("attestations")
    if (review.get("kind") != "human" or review.get("decision") != "approve"
            or review.get("packet_sha256") != digest(packet_path)
            or not isinstance(attestations, dict)
            or any(attestations.get(key) is not True for key in ATTESTATIONS)):
        raise ValueError("human review must approve these exact bytes and all journey attestations")
    return platform, identity


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("packets", nargs=2, type=Path)
    args = parser.parse_args(argv)
    try:
        if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
            raise ValueError("source SHA must be 40 lowercase hexadecimal characters")
        results = [verify(path, args.source_sha) for path in args.packets]
        if {platform for platform, _ in results} != {"linux", "windows"}:
            raise ValueError("one Linux and one Windows packet are required")
        if results[0][1] != results[1][1]:
            raise ValueError("platform plan/source/profile receipt identities differ")
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"Operator acceptance rejected: {error}", file=sys.stderr)
        return 1
    print(f"Operator packet integrity and recorded human approvals verified for {args.source_sha}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
