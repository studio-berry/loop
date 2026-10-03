#!/usr/bin/env python3
"""Release-gate contract for GitHub #241 independent validation lanes.

This guard keeps the independent-oracle scaffolding reviewable in CI: conversion
fixture triads, the external-validator helper, oracle unit tests, and the Core
source seams that forbid self-certification must remain present.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

SESSION_MANIFEST = ROOT / "docs/evidence/session-10-trust/conversion-fixture-manifest.json"
CONVERSION_MANIFEST = ROOT / "loop-preflight/testdata/conversion/manifest.json"
VALIDATOR_SCRIPT = ROOT / "scripts/qualification/run_independent_validators.py"
STANDARD_CONVERSION = ROOT / "LoopLibCore/sources/pdfstandardconversion.cpp"
REPAIR_PRIMITIVES = ROOT / "LoopLibCore/sources/pdfrepairprimitives.cpp"
CMAKE = ROOT / "UnitTests/CMakeLists.txt"
CLAIM_MANIFEST = ROOT / "docs/independent-claims.json"
CLAIM_REGISTRY = ROOT / "LoopLibCore/sources/pdfconformanceclaim.cpp"
INDEPENDENT_DOC = ROOT / "docs/INDEPENDENT_VALIDATION.md"

REQUIRED_ORACLE_TESTS = (
    "UnitTestsStandardOracle",
    "UnitTestsConversionOracle",
)

REQUIRED_MARKERS = {
    STANDARD_CONVERSION: (
        "independentValidatorProgram",
        "independent_validation_passed",
        "An independent validator is required",
    ),
    REPAIR_PRIMITIVES: (
        "independent validator",
        "{input}",
    ),
}


def _load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def inventory_failures() -> list[str]:
    if not CLAIM_MANIFEST.is_file():
        return ["missing independent claim inventory"]
    manifest = _load_json(CLAIM_MANIFEST)
    failures = []
    rows = manifest.get("claims", [])
    if len({row["id"] for row in rows}) != len(rows):
        failures.append("duplicate independent claim id")
    source = STANDARD_CONVERSION.read_text(encoding="utf-8")
    targets = source.split("QStringList supportedPDFStandardTargets()", 1)[1].split("QJsonObject", 1)[0]
    registered = set(re.findall(r'QStringLiteral\("([^"\n]+)"\)', targets))
    if set(manifest["conversion_targets"]) != registered:
        failures.append("conversion target inventory drift")
    registry = CLAIM_REGISTRY.read_text(encoding="utf-8").split("const PDFConformanceClaim kConformanceClaims[]", 1)[1].split("};", 1)[0]
    unsupported = set(re.findall(r'\{ QLatin1String\("([^"\n]+)"\)', registry))
    if unsupported != set(manifest["declared_unsupported"]):
        failures.append("unsupported conformance inventory drift")
    renderer = (ROOT / "LoopLibCore/sources/pdftransparencyrenderer.h").read_text(encoding="utf-8")
    purposes = renderer.split("enum class PDFRenderPurpose", 1)[1].split("};", 1)[0]
    purpose_names = set(re.findall(r"^\s*([A-Za-z]+),?\s*$", purposes, re.MULTILINE))
    if set(manifest["renderer_purposes"]) != purpose_names or not set(manifest["renderer_purposes"].values()).issubset({row["id"] for row in rows}):
        failures.append("renderer policy inventory drift")
    row_targets = {row["target"] for row in rows}
    if not registered.union(unsupported).issubset(row_targets):
        failures.append("a conformance target has no oracle or limitation")
    if not {"structure", "signature", "conversion", "authoritative-rendering", "canvas-rendering"}.issubset({row["id"] for row in rows}):
        failures.append("missing independent claim family")
    for row in rows:
        if not row.get("surfaces") or not row.get("limitations") or (not row.get("oracle") and not row.get("enforcement")):
            failures.append(f"claim {row['id']} has no scoped oracle or enforced limitation")
    known = {fixture["id"] for fixture in manifest["fixtures"] + manifest["derived_fixtures"]}
    for row in rows:
        if not set(row["fixtures"]).issubset(known) or not row.get("expected") or not row.get("scope"):
            failures.append(f"claim {row['id']} has dangling fixtures or no expected scope")
    for fixture in manifest["derived_fixtures"]:
        if fixture["source"] not in known or not fixture.get("generator") or not fixture.get("limitations"):
            failures.append(f"derived fixture {fixture['id']} lacks source or generator")
    for fixture in manifest["fixtures"]:
        path = ROOT / fixture["path"]
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != fixture["sha256"]:
            failures.append(f"fixture digest mismatch: {fixture['id']}")
    return failures


def main() -> int:
    failures: list[str] = inventory_failures()

    for path in (SESSION_MANIFEST, CONVERSION_MANIFEST, VALIDATOR_SCRIPT, INDEPENDENT_DOC):
        if not path.is_file():
            failures.append(f"missing required artifact: {path.relative_to(ROOT)}")

    if SESSION_MANIFEST.is_file():
        session = _load_json(SESSION_MANIFEST)
        triad = session.get("conversion_triad")
        if not isinstance(triad, list) or len(triad) != 3:
            failures.append(f"{SESSION_MANIFEST.relative_to(ROOT)}: conversion_triad must contain exactly 3 fixtures")
        else:
            kinds = {item.get("kind") for item in triad if isinstance(item, dict)}
            for required in ("already-conformant", "safely-convertible", "deliberately-unconvertible"):
                if required not in kinds:
                    failures.append(f"{SESSION_MANIFEST.relative_to(ROOT)}: missing triad kind {required!r}")

    if CONVERSION_MANIFEST.is_file():
        conversion = _load_json(CONVERSION_MANIFEST)
        fixtures = conversion.get("fixtures")
        if not isinstance(fixtures, list) or len(fixtures) < 3:
            failures.append(f"{CONVERSION_MANIFEST.relative_to(ROOT)}: fixtures must list the conversion triad")

    cmake_text = CMAKE.read_text(encoding="utf-8")
    for test_name in REQUIRED_ORACLE_TESTS:
        if test_name not in cmake_text:
            failures.append(f"{CMAKE.relative_to(ROOT)}: missing oracle test target {test_name}")

    for path, markers in REQUIRED_MARKERS.items():
        if not path.is_file():
            failures.append(f"missing required source: {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                failures.append(f"{path.relative_to(ROOT)}: missing marker {marker!r}")

    if failures:
        print("ERROR: independent validation release gate failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(
        "Independent validation source gate passed; runtime qualification is not asserted. Oracle manifests, helper script, "
        "unit tests, and self-certification guards are present."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
