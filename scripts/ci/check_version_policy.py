#!/usr/bin/env python3
"""Keep CMake, release tagging, and documented SemVer policy aligned."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = ROOT / "docs" / "version-policy.json"
VERSIONING_DOC = ROOT / "docs" / "VERSIONING.md"
CMAKE_LISTS = ROOT / "CMakeLists.txt"
APPX_MANIFEST = ROOT / "AppxManifest.xml.in"
RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "CreateReleaseDraft.yml"
AGENTS_MD = ROOT / "AGENTS.md"

SEMVER_CORE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
CMAKE_VERSION = re.compile(
    r"^set\(LOOP_VERSION\s+([0-9]+(?:\.[0-9]+)*)\)\s*$", re.MULTILINE
)
CMAKE_PRERELEASE = re.compile(
    r"^set\(LOOP_VERSION_PRERELEASE\s+([^)]*)\)\s*$", re.MULTILINE
)
FOUR_PART_RELEASE_GREP = re.compile(
    r"set\\\(LOOP_VERSION \\K\[0-9\]\+\\.\[0-9\]\+\\.\[0-9\]\+\\.\[0-9\]\+"
)
THREE_PART_RELEASE_GREP = re.compile(
    r"set\\\(LOOP_VERSION \\K\[0-9\]\+\\.\[0-9\]\+\\.\[0-9\]\+"
)
PRERELEASE_RELEASE_GREP = re.compile(r"LOOP_VERSION_PRERELEASE")

DOCUMENTED_SCHEME = re.compile(r"^[-*]\s+Scheme:\s*(.+)$", re.MULTILINE)
DOCUMENTED_CANONICAL = re.compile(r"^[-*]\s+Canonical version:\s*(.+)$", re.MULTILINE)
DOCUMENTED_FORMAT = re.compile(r"^[-*]\s+Format:\s*(.+)$", re.MULTILINE)
DOCUMENTED_CURRENT = re.compile(r"^[-*]\s+Current version:\s*(.+)$", re.MULTILINE)
DOCUMENTED_PRERELEASE = re.compile(r"^[-*]\s+Pre-release:\s*(.+)$", re.MULTILINE)
DOCUMENTED_TAGS = re.compile(r"^[-*]\s+Git tags:\s*(.+)$", re.MULTILINE)
DOCUMENTED_WINDOWS = re.compile(
    r"^[-*]\s+Windows Appx version:\s*(.+)$", re.MULTILINE
)
DOCUMENTED_RELEASE_WORKFLOW = re.compile(
    r"^[-*]\s+Release workflow:\s*`([^`]+)`$", re.MULTILINE
)


def parse_policy(text: str) -> dict[str, str]:
    policy = json.loads(text)
    required = {
        "scheme",
        "spec",
        "cmake_variable",
        "cmake_format",
        "current",
        "tag_prefix",
        "windows_variable",
        "windows_four_part",
        "release_workflow",
    }
    missing = sorted(required - policy.keys())
    if missing:
        raise ValueError(f"version policy is missing: {', '.join(missing)}")
    for key in required:
        value = policy[key]
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"version policy {key} must be a non-empty string")
    prerelease = policy.get("prerelease", "")
    if not isinstance(prerelease, str):
        raise ValueError("version policy prerelease must be a string")
    policy["prerelease"] = prerelease.strip()
    if policy["scheme"] != "semver":
        raise ValueError(f"version policy scheme must be semver, got {policy['scheme']!r}")
    if policy["cmake_format"] != "MAJOR.MINOR.PATCH":
        raise ValueError("version policy cmake_format must be MAJOR.MINOR.PATCH")
    if policy["tag_prefix"] != "v":
        raise ValueError("version policy tag_prefix must be v")
    if not SEMVER_CORE.fullmatch(policy["current"]):
        raise ValueError(
            f"version policy current must be MAJOR.MINOR.PATCH, got {policy['current']!r}"
        )
    return policy


def parse_cmake_version(text: str) -> str:
    match = CMAKE_VERSION.search(text)
    if not match:
        raise ValueError("CMakeLists.txt has no set(LOOP_VERSION ...) assignment")
    version = match.group(1)
    if not SEMVER_CORE.fullmatch(version):
        raise ValueError(
            f"LOOP_VERSION must be SemVer MAJOR.MINOR.PATCH without a fourth "
            f"component or pre-release suffix, got {version!r}"
        )
    return version


def parse_cmake_prerelease(text: str) -> str:
    match = CMAKE_PRERELEASE.search(text)
    if not match:
        return ""
    return match.group(1).strip()


def validate_versioning_doc(text: str, policy: dict[str, str]) -> list[str]:
    errors: list[str] = []

    def required(pattern: re.Pattern[str], label: str) -> str | None:
        match = pattern.search(text)
        if not match:
            errors.append(f"VERSIONING.md is missing {label}")
            return None
        return match.group(1).strip()

    scheme = required(DOCUMENTED_SCHEME, "Scheme")
    if scheme and "semver" not in scheme.lower():
        errors.append(f"VERSIONING.md Scheme must name SemVer 2.0, got {scheme!r}")

    canonical = required(DOCUMENTED_CANONICAL, "Canonical version")
    if canonical and policy["cmake_variable"] not in canonical:
        errors.append("VERSIONING.md Canonical version must name LOOP_VERSION")

    fmt = required(DOCUMENTED_FORMAT, "Format")
    if fmt and fmt != policy["cmake_format"]:
        errors.append(f"VERSIONING.md Format must be {policy['cmake_format']}")

    current = required(DOCUMENTED_CURRENT, "Current version")
    if current and current != policy["current"]:
        errors.append(
            f"VERSIONING.md Current version must be {policy['current']}, got {current!r}"
        )

    prerelease = required(DOCUMENTED_PRERELEASE, "Pre-release")
    if prerelease is not None and prerelease != (policy["prerelease"] or "none"):
        if prerelease != policy["prerelease"]:
            errors.append(
                f"VERSIONING.md Pre-release must be {policy['prerelease'] or 'none'}, "
                f"got {prerelease!r}"
            )

    tags = required(DOCUMENTED_TAGS, "Git tags")
    if tags and not tags.startswith(f"`{policy['tag_prefix']}"):
        errors.append("VERSIONING.md Git tags must use the v prefix")

    windows = required(DOCUMENTED_WINDOWS, "Windows Appx version")
    if windows and policy["windows_variable"] not in windows:
        errors.append("VERSIONING.md Windows Appx version must name LOOP_WINDOWS_VERSION")

    workflow = required(DOCUMENTED_RELEASE_WORKFLOW, "Release workflow")
    if workflow and workflow != policy["release_workflow"]:
        errors.append(
            f"VERSIONING.md Release workflow must be {policy['release_workflow']}"
        )
    return errors


def validate_release_workflow(text: str, prerelease: str = "") -> list[str]:
    errors: list[str] = []
    if FOUR_PART_RELEASE_GREP.search(text):
        errors.append(
            "CreateReleaseDraft.yml still parses a four-part LOOP_VERSION; "
            "use MAJOR.MINOR.PATCH"
        )
    elif not THREE_PART_RELEASE_GREP.search(text):
        errors.append(
            "CreateReleaseDraft.yml must grep set(LOOP_VERSION) as MAJOR.MINOR.PATCH"
        )
    if prerelease and not PRERELEASE_RELEASE_GREP.search(text):
        errors.append(
            "CreateReleaseDraft.yml must read LOOP_VERSION_PRERELEASE when a "
            "pre-release label is set"
        )
    return errors


def validate_appx_manifest(text: str, policy: dict[str, str]) -> list[str]:
    needle = "${" + policy["windows_variable"] + "}"
    if needle not in text:
        return [
            f"AppxManifest.xml.in Identity.Version must use {policy['windows_variable']}"
        ]
    if "${LOOP_VERSION}" in text:
        return ["AppxManifest.xml.in must not use three-part LOOP_VERSION"]
    return []


def validate_agents_version(text: str, version: str, prerelease: str = "") -> list[str]:
    display = f"{version}-{prerelease}" if prerelease else version
    if f"`{display}`" not in text and f"`{version}`" not in text:
        return [f"AGENTS.md must list the current version `{display}`"]
    if re.search(r"`[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+`", text):
        return ["AGENTS.md still lists a four-part product version"]
    return []


def validate_cmake_windows_version(text: str, policy: dict[str, str]) -> list[str]:
    if policy["windows_variable"] not in text:
        return [f"CMakeLists.txt must define {policy['windows_variable']}"]
    return []


def validate_repository(root: Path) -> list[str]:
    errors: list[str] = []
    try:
        policy = parse_policy((root / POLICY_PATH.relative_to(ROOT)).read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return [f"docs/version-policy.json: {exc}"]

    cmake_text = (root / CMAKE_LISTS.relative_to(ROOT)).read_text(encoding="utf-8")
    try:
        version = parse_cmake_version(cmake_text)
    except (OSError, ValueError) as exc:
        errors.append(str(exc))
        version = None
    prerelease = parse_cmake_prerelease(cmake_text)
    if version is not None and version != policy["current"]:
        errors.append(
            f"LOOP_VERSION {version!r} must match version-policy.json current "
            f"{policy['current']!r}"
        )
    if prerelease != policy["prerelease"]:
        errors.append(
            f"LOOP_VERSION_PRERELEASE {prerelease!r} must match version-policy.json "
            f"prerelease {policy['prerelease']!r}"
        )

    errors.extend(validate_cmake_windows_version(cmake_text, policy))
    errors.extend(
        validate_versioning_doc(
            (root / VERSIONING_DOC.relative_to(ROOT)).read_text(encoding="utf-8"), policy
        )
    )
    errors.extend(
        validate_release_workflow(
            (root / RELEASE_WORKFLOW.relative_to(ROOT)).read_text(encoding="utf-8"),
            policy["prerelease"],
        )
    )
    errors.extend(
        validate_appx_manifest(
            (root / APPX_MANIFEST.relative_to(ROOT)).read_text(encoding="utf-8"), policy
        )
    )
    if version is not None:
        errors.extend(
            validate_agents_version(
                (root / AGENTS_MD.relative_to(ROOT)).read_text(encoding="utf-8"),
                version,
                prerelease,
            )
        )
    expected_workflow = policy["release_workflow"].replace("\\", "/")
    if expected_workflow != ".github/workflows/CreateReleaseDraft.yml":
        errors.append(
            f"version-policy.json release_workflow must be "
            f".github/workflows/CreateReleaseDraft.yml, got {expected_workflow}"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    errors = validate_repository(ROOT)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("version policy ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
