#!/usr/bin/env python3
"""Generate and validate the repository's architecture fact inventory.

The generated JSON is deliberately derived from source and CMake rather than
being a second hand-maintained list.  Use ``--write`` when source changes, and
``--check`` in CI to fail when the committed copy is stale or an ADR is missing
its verification header.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "docs" / "generated" / "architecture-catalog.json"
BRANCH_POLICY_PATH = ROOT / "docs" / "branch-policy.json"
VERSION_POLICY_PATH = ROOT / "docs" / "version-policy.json"
ADR_DIR = ROOT / "docs" / "adr"

FULL_SHA = re.compile(r"^[0-9a-f]{40}$")
LAST_VERIFIED = re.compile(r"^\d{4}-\d{2}-\d{2} @ ([0-9a-f]{40})$")
HEADER_LINE = re.compile(r"^\*\*(Status|Implemented-at|Last-verified|Superseded-by):\*\* (.+)$")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def unique_sorted(values: Iterable[str]) -> list[str]:
    return sorted(set(values), key=lambda value: value.casefold())


def parse_branch_policy() -> dict[str, Any]:
    policy = json.loads(read(BRANCH_POLICY_PATH))
    required = {
        "default_branch",
        "release_branch",
        "integration_branch",
        "topic_branch_source",
        "topic_branch_patterns",
        "protected_branches",
    }
    missing = sorted(required - policy.keys())
    if missing:
        raise ValueError(f"branch policy is missing: {', '.join(missing)}")
    branches = {
        policy["default_branch"],
        policy["release_branch"],
        policy["integration_branch"],
        policy["topic_branch_source"],
        *policy["protected_branches"],
    }
    if any(not isinstance(branch, str) or not branch for branch in branches):
        raise ValueError("branch policy contains an empty branch name")
    if not isinstance(policy["topic_branch_patterns"], list) or not all(
        isinstance(pattern, str) and pattern for pattern in policy["topic_branch_patterns"]
    ):
        raise ValueError("topic_branch_patterns must be a non-empty string list")
    return {
        "default": policy["default_branch"],
        "release": policy["release_branch"],
        "integration": policy["integration_branch"],
        "topic_source": policy["topic_branch_source"],
        "protected": sorted(policy["protected_branches"]),
        "topic_patterns": sorted(policy["topic_branch_patterns"]),
    }


def parse_version_policy() -> dict[str, Any]:
    policy = json.loads(read(VERSION_POLICY_PATH))
    required = {
        "scheme",
        "current",
        "tag_prefix",
        "cmake_format",
    }
    missing = sorted(required - policy.keys())
    if missing:
        raise ValueError(f"version policy is missing: {', '.join(missing)}")
    if policy["scheme"] != "semver":
        raise ValueError("version policy scheme must be semver")
    prerelease = policy.get("prerelease") or ""
    if not isinstance(prerelease, str):
        raise ValueError("version policy prerelease must be a string")
    return {
        "scheme": policy["scheme"],
        "current": policy["current"],
        "prerelease": prerelease or None,
        "tag_prefix": policy["tag_prefix"],
        "cmake_format": policy["cmake_format"],
    }


def parse_preflight_checks() -> list[str]:
    source = read(ROOT / "Pdf4QtLibCore" / "sources" / "preflightengine.cpp")
    match = re.search(
        r"void\s+PreflightEngine::registerBuiltInChecks\(\)\s*\{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not match:
        raise ValueError("could not find PreflightEngine::registerBuiltInChecks")
    ids = re.findall(r'QStringLiteral\("([a-z][a-z0-9-]*)"\)', match.group("body"))
    checks = unique_sorted(ids)
    if not checks:
        raise ValueError("preflight check catalog is empty")
    return checks


def parse_repair_operations() -> list[dict[str, str]]:
    operations: list[dict[str, str]] = []
    pattern = re.compile(
        r"class\s+(\w+)\s+final\s*:\s*public\s+PDFRepairOperation.*?"
        r"QString\s+id\(\)\s+const\s+override\s*\{\s*"
        r'return\s+QStringLiteral\("([^"]+)"\)',
        re.DOTALL,
    )
    for path in sorted((ROOT / "Pdf4QtLibCore" / "sources").glob("*.cpp")):
        for class_name, operation_id in pattern.findall(read(path)):
            operations.append(
                {"id": operation_id, "implementation": path.relative_to(ROOT).as_posix()}
            )
    operations.sort(key=lambda operation: operation["id"])
    if not operations:
        raise ValueError("registered operation catalog is empty")
    if len({operation["id"] for operation in operations}) != len(operations):
        raise ValueError("registered operation catalog contains duplicate ids")
    return operations


def schema_version_values(value: Any) -> list[int]:
    """Collect values belonging to properties named exactly schema_version."""
    if isinstance(value, dict):
        result: list[int] = []
        for key, child in value.items():
            if key == "schema_version" and isinstance(child, dict):
                for version_key in ("const", "default"):
                    version = child.get(version_key)
                    if isinstance(version, int) and not isinstance(version, bool):
                        result.append(version)
                enum = child.get("enum")
                if isinstance(enum, list):
                    result.extend(
                        item for item in enum
                        if isinstance(item, int) and not isinstance(item, bool)
                    )
            else:
                result.extend(schema_version_values(child))
        return result
    if isinstance(value, list):
        result: list[int] = []
        for child in value:
            result.extend(schema_version_values(child))
        return result
    return []


def parse_schema_kinds() -> list[str]:
    source = read(ROOT / "Pdf4QtLibCore" / "sources" / "pdfschemaversion.cpp")
    match = re.search(
        r"QString\s+pdfSchemaKindToString\(PDFSchemaKind kind\)\s*\{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not match:
        raise ValueError("could not find pdfSchemaKindToString")
    kinds = unique_sorted(re.findall(r'return QStringLiteral\("([a-z0-9-]+)"\)', match.group("body")))
    kinds = [kind for kind in kinds if kind != "unknown"]
    if len(kinds) < 10:
        raise ValueError("schema kind catalog is incomplete")
    return kinds


def parse_architecture_invariants() -> list[dict[str, Any]]:
    path = ROOT / "docs" / "architecture-invariants.json"
    document = json.loads(read(path))
    invariants = document.get("invariants")
    if not isinstance(invariants, list) or not invariants:
        raise ValueError("architecture invariants are missing")
    tests = set(parse_test_targets())
    for invariant in invariants:
        identifier = invariant.get("id")
        mapped = invariant.get("tests")
        if not identifier or not isinstance(mapped, list) or not mapped:
            raise ValueError(f"invariant {identifier!r} is missing tests")
        missing = [name for name in mapped if name not in tests]
        if missing:
            raise ValueError(f"{identifier} maps to unknown tests: {', '.join(missing)}")
    return invariants


def parse_coverage_matrix() -> dict[str, Any]:
    checks = parse_preflight_checks()
    families = {
        "images": ["image-resolution"],
        "colorants": ["color-mode", "color-inventory", "output-intent"],
        "strokes": ["thin-strokes", "thin-parts"],
        "overprint-transparency": ["white-overprint", "transparency-risk"],
        "fonts": ["embedded-fonts", "font-integrity"],
    }
    covered = {check for members in families.values() for check in members}
    holes = [check for check in checks if check not in covered]
    return {
        "families": families,
        "coverage_holes": holes,
        "standards_matrix": "docs/PDFX_POLICY_MATRIX.md",
    }


def parse_schema_versions() -> dict[str, Any]:
    schemas: dict[str, Any] = {}
    schema_dir = ROOT / "loupe-preflight" / "schemas"
    for path in sorted(schema_dir.glob("*.json")):
        document = json.loads(read(path))
        versions = unique_sorted(str(value) for value in schema_version_values(document))
        schemas[path.relative_to(ROOT).as_posix()] = [int(value) for value in versions]

    engine_header = read(ROOT / "Pdf4QtLibCore" / "sources" / "preflightengine.h")
    report_match = re.search(r"PREFLIGHT_REPORT_SCHEMA_VERSION\s*=\s*(\d+)", engine_header)
    engine_cpp = read(ROOT / "Pdf4QtLibCore" / "sources" / "preflightengine.cpp")
    decision_match = re.search(
        r'preflightDecisionsToJson.*?schema_version"\),\s*(\d+)', engine_cpp, re.DOTALL
    )
    action_list = read(ROOT / "Pdf4QtLibCore" / "sources" / "pdfactionlist.cpp")
    action_match = re.search(r'loupe-action-list/(\d+)', action_list)
    if not report_match or not decision_match or not action_match:
        raise ValueError("could not find one or more runtime schema versions")
    return {
        "runtime": {
            "preflight_report": int(report_match.group(1)),
            "preflight_decisions": int(decision_match.group(1)),
            "action_list": int(action_match.group(1)),
        },
        "json_schemas": schemas,
    }


def parse_test_targets() -> list[str]:
    source = read(ROOT / "UnitTests" / "CMakeLists.txt")
    targets = re.findall(r"add_executable\(\s*(UnitTests[A-Za-z0-9_]*)\b", source)
    targets = unique_sorted(targets)
    if not targets:
        raise ValueError("CMake test target catalog is empty")
    return targets


def parse_workflow_branches() -> dict[str, list[str]]:
    """Extract branch trigger lists without requiring a YAML dependency."""
    workflows: dict[str, list[str]] = {}
    for path in sorted((ROOT / ".github" / "workflows").glob("*.yml")):
        lines = read(path).splitlines()
        branches: list[str] = []
        index = 0
        while index < len(lines):
            match = re.match(r"^\s*branches:\s*(.*)$", lines[index])
            if not match:
                index += 1
                continue
            inline = match.group(1).strip()
            if inline.startswith("[") and inline.endswith("]"):
                branches.extend(
                    item.strip().strip("'\"")
                    for item in inline[1:-1].split(",")
                    if item.strip()
                )
            else:
                child = index + 1
                while child < len(lines):
                    item_match = re.match(r"^\s+-\s+([^\s#]+)", lines[child])
                    if not item_match:
                        break
                    branches.append(item_match.group(1).strip("'\""))
                    child += 1
                index = child - 1
            index += 1
        if branches:
            workflows[path.relative_to(ROOT).as_posix()] = unique_sorted(branches)
    return workflows


def validate_adrs() -> list[str]:
    errors: list[str] = []
    allowed_statuses = {"proposed", "accepted", "implemented", "superseded"}
    for path in sorted(ADR_DIR.glob("*.md")):
        headers: dict[str, str] = {}
        for line in read(path).splitlines():
            if line.startswith("## "):
                break
            match = HEADER_LINE.match(line)
            if match:
                headers[match.group(1)] = match.group(2).strip()
        required = {"Status", "Implemented-at", "Last-verified", "Superseded-by"}
        missing = sorted(required - headers.keys())
        if missing:
            errors.append(f"{path.relative_to(ROOT)} missing: {', '.join(missing)}")
            continue
        status = headers["Status"].lower()
        if status not in allowed_statuses:
            errors.append(f"{path.relative_to(ROOT)} has invalid Status: {headers['Status']}")
        implemented_at = headers["Implemented-at"]
        if status == "implemented" and not FULL_SHA.fullmatch(implemented_at):
            errors.append(f"{path.relative_to(ROOT)} implemented ADR has invalid Implemented-at")
        if status != "implemented" and not implemented_at:
            errors.append(f"{path.relative_to(ROOT)} has empty Implemented-at")
        if not LAST_VERIFIED.fullmatch(headers["Last-verified"]):
            errors.append(f"{path.relative_to(ROOT)} has invalid Last-verified")
        if status == "superseded" and headers["Superseded-by"].lower() in {"none", "n/a", "not applicable"}:
            errors.append(f"{path.relative_to(ROOT)} is superseded without a successor")
    return errors


def build_catalog() -> dict[str, Any]:
    return {
        "format_version": 1,
        "generated_by": "scripts/generate-architecture-catalogs.py",
        "branch_policy": parse_branch_policy(),
        "version_policy": parse_version_policy(),
        "preflight_checks": parse_preflight_checks(),
        "registered_operations": parse_repair_operations(),
        "schema_versions": parse_schema_versions(),
        "schema_kinds": parse_schema_kinds(),
        "architecture_invariants": parse_architecture_invariants(),
        "preflight_coverage": parse_coverage_matrix(),
        "test_targets": parse_test_targets(),
        "workflow_branches": parse_workflow_branches(),
        "sources": [
            "docs/branch-policy.json",
            "docs/version-policy.json",
            "Pdf4QtLibCore/sources/preflightengine.cpp",
            "Pdf4QtLibCore/sources/preflightengine.h",
            "Pdf4QtLibCore/sources/pdfactionlist.cpp",
            "Pdf4QtLibCore/sources/pdfrepairoperation.cpp",
            "Pdf4QtLibCore/sources/pdfrepairprimitives.cpp",
            "Pdf4QtLibCore/sources/pdfproductionrepair.cpp",
            "loupe-preflight/schemas/*.json",
            "Pdf4QtLibCore/sources/pdfschemaversion.cpp",
            "docs/architecture-invariants.json",
            "docs/PDFX_POLICY_MATRIX.md",
            "UnitTests/CMakeLists.txt",
            ".github/workflows/*.yml",
        ],
    }


def serialized_catalog() -> str:
    return json.dumps(build_catalog(), indent=2, sort_keys=True) + "\n"


def check_catalog(expected: str) -> int:
    if not CATALOG_PATH.exists():
        print(f"error: generated catalog is missing: {CATALOG_PATH.relative_to(ROOT)}", file=sys.stderr)
        return 1
    actual = read(CATALOG_PATH)
    if actual == expected:
        return 0
    diff = difflib.unified_diff(
        actual.splitlines(),
        expected.splitlines(),
        fromfile=str(CATALOG_PATH.relative_to(ROOT)),
        tofile="generated output",
        lineterm="",
    )
    print("generated architecture catalog is stale:", file=sys.stderr)
    print("\n".join(diff), file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="validate ADRs and the committed catalog")
    mode.add_argument("--write", action="store_true", help="validate ADRs and write the catalog")
    args = parser.parse_args()

    errors = validate_adrs()
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    try:
        expected = serialized_catalog()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: cannot generate architecture catalog: {error}", file=sys.stderr)
        return 1

    if args.write:
        CATALOG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CATALOG_PATH.write_text(expected, encoding="utf-8", newline="\n")
        return 0
    return check_catalog(expected)


if __name__ == "__main__":
    raise SystemExit(main())
