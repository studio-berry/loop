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
PREFLIGHT_CATALOG_PATH = ROOT / "docs" / "generated" / "preflight-check-catalog.json"
PREFLIGHT_BACKLOG_PATH = ROOT / "docs" / "generated" / "preflight-coverage-backlog.json"
PREFLIGHT_OVERLAY_PATH = ROOT / "docs" / "preflight-check-catalog-overlay.json"
CORRECTION_CATALOG_PATH = ROOT / "docs" / "generated" / "correction-operation-catalog.json"
CORRECTION_OVERLAY_PATH = ROOT / "docs" / "correction-operation-catalog-overlay.json"
BRANCH_POLICY_PATH = ROOT / "docs" / "branch-policy.json"
VERSION_POLICY_PATH = ROOT / "docs" / "version-policy.json"
INVARIANTS_PATH = ROOT / "docs" / "architecture-invariants.json"
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
        "qualification_branch",
        "topic_branch_source",
        "topic_branch_patterns",
        "promotion_chain",
        "protected_branches",
    }
    missing = sorted(required - policy.keys())
    if missing:
        raise ValueError(f"branch policy is missing: {', '.join(missing)}")
    branches = {
        policy["default_branch"],
        policy["release_branch"],
        policy["integration_branch"],
        policy["qualification_branch"],
        policy["topic_branch_source"],
        *policy["protected_branches"],
        *policy["promotion_chain"],
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
        "qualification": policy["qualification_branch"],
        "topic_source": policy["topic_branch_source"],
        "promotion_chain": policy["promotion_chain"],
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
    source = read(ROOT / "LoopLibCore" / "sources" / "preflightengine.cpp")
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


PREFLIGHT_CHECK_ENVELOPE_FIELDS = {"id", "severity", "enabled"}
PREFLIGHT_PARAMETER_TYPES = {"number", "integer", "boolean", "string", "string-list", "object"}
PREFLIGHT_PARAMETER_FIELDS = ("id", "type", "default", "range", "meaning")
PREFLIGHT_SEVERITY_FIELDS = ("finding_type", "severity", "condition")
PREFLIGHT_SEVERITY_VALUES = {"error", "warning", "info"}
PREFLIGHT_FINDING_FIELDS = {
    "scope",
    "page",
    "type",
    "severity",
    "message",
    "check_id",
    "object_id",
    "bbox",
    "evidence_ids",
}
PREFLIGHT_BACKLOG_FIELDS = ("id", "priority", "gap", "families", "state", "closed_by")
PREFLIGHT_BACKLOG_PRIORITIES = {"P1", "P2", "P3"}
PREFLIGHT_BACKLOG_STATES = {"open", "landed", "closed"}
PREFLIGHT_BACKLOG_UNFILED = "unfiled"
GITHUB_ISSUE_REF = re.compile(r"#[1-9][0-9]*")
CHECK_ID = re.compile(r"^[a-z][a-z0-9-]*$")


def parse_profile_check_field_names() -> set[str]:
    """Profile fields the engine's own check parser reads from a profile row.

    Scraped from the ``checks`` loop of ``PreflightEngine::parseProfile`` rather
    than from the published profile schema, so a field the engine accepts but the
    schema does not yet name (``required_types``, ``raster_white_threshold``) is
    still a legal catalog parameter id. Check ``id``/``severity``/``enabled`` are
    the envelope every check shares and are described by the ``severity`` block,
    so they are excluded.
    """
    source = read(ROOT / "LoopLibCore" / "sources" / "preflightengine.cpp")
    marker = 'const QJsonArray checks = profileObject.value(QStringLiteral("checks")).toArray();'
    start = source.find(marker)
    if start < 0:
        raise ValueError("could not find the profile checks loop in preflightengine.cpp")
    end = source.find("profile.checks.push_back(check);", start)
    if end < 0:
        raise ValueError("could not find the end of the profile checks loop")
    body = source[start:end]
    names = set(re.findall(r'checkObject\.(?:value|contains)\(QStringLiteral\("([a-z0-9_]+)"\)\)', body))
    names.add("restrictions")
    names -= PREFLIGHT_CHECK_ENVELOPE_FIELDS
    if len(names) < 20:
        raise ValueError("profile check field scrape is incomplete")
    return names


def preflight_fixup_ids(fixup_registry: list[dict[str, Any]]) -> list[str]:
    """Registered preflight fixup ids, from the repair-operation registry."""
    return unique_sorted(operation["id"] for operation in fixup_registry if operation["is_preflight_fixup"])


def validate_check_parameters(check_id: str, parameters: Any, known_fields: set[str]) -> None:
    if not isinstance(parameters, list):
        raise ValueError(f"catalog entry '{check_id}' parameters must be an array")
    seen: set[str] = set()
    for parameter in parameters:
        if not isinstance(parameter, dict):
            raise ValueError(f"catalog entry '{check_id}' has a non-object parameter")
        absent = sorted(set(PREFLIGHT_PARAMETER_FIELDS) - set(parameter))
        extra = sorted(set(parameter) - set(PREFLIGHT_PARAMETER_FIELDS))
        if absent or extra:
            raise ValueError(
                f"catalog entry '{check_id}' parameter must carry exactly "
                f"{', '.join(PREFLIGHT_PARAMETER_FIELDS)}"
            )
        identifier = parameter["id"]
        if identifier not in known_fields:
            raise ValueError(
                f"catalog entry '{check_id}' parameter '{identifier}' is not a profile check field the engine reads"
            )
        if identifier in seen:
            raise ValueError(f"catalog entry '{check_id}' repeats parameter '{identifier}'")
        seen.add(identifier)
        if parameter["type"] not in PREFLIGHT_PARAMETER_TYPES:
            raise ValueError(f"catalog entry '{check_id}' parameter '{identifier}' has invalid type")
        if not isinstance(parameter["range"], str) or not parameter["range"].strip():
            raise ValueError(f"catalog entry '{check_id}' parameter '{identifier}' needs a range string")
        if not isinstance(parameter["meaning"], str) or not parameter["meaning"].strip():
            raise ValueError(f"catalog entry '{check_id}' parameter '{identifier}' needs a meaning")
        default = parameter["default"]
        if not isinstance(default, (str, int, float, bool, list, dict)) and default is not None:
            raise ValueError(f"catalog entry '{check_id}' parameter '{identifier}' has an unsupported default")


def validate_check_severity(check_id: str, severity: Any) -> None:
    if not isinstance(severity, list) or not severity:
        raise ValueError(f"catalog entry '{check_id}' severity must be a non-empty array")
    for entry in severity:
        if not isinstance(entry, dict):
            raise ValueError(f"catalog entry '{check_id}' has a non-object severity entry")
        absent = sorted(set(PREFLIGHT_SEVERITY_FIELDS) - set(entry))
        extra = sorted(set(entry) - set(PREFLIGHT_SEVERITY_FIELDS))
        if absent or extra:
            raise ValueError(
                f"catalog entry '{check_id}' severity entry must carry exactly "
                f"{', '.join(PREFLIGHT_SEVERITY_FIELDS)}"
            )
        finding_types = entry["finding_type"]
        if isinstance(finding_types, str):
            finding_types = [finding_types]
        if not isinstance(finding_types, list) or not finding_types or not all(
            isinstance(item, str) and item.strip() for item in finding_types
        ):
            raise ValueError(f"catalog entry '{check_id}' severity entry needs finding_type names")
        if entry["severity"] not in PREFLIGHT_SEVERITY_VALUES:
            raise ValueError(f"catalog entry '{check_id}' severity entry has an invalid severity")
        if not isinstance(entry["condition"], str) or not entry["condition"].strip():
            raise ValueError(f"catalog entry '{check_id}' severity entry needs a condition")


def validate_check_evidence(check_id: str, evidence: Any) -> None:
    if not isinstance(evidence, list) or not evidence:
        raise ValueError(f"catalog entry '{check_id}' evidence must be a non-empty array")
    seen: set[str] = set()
    for name in evidence:
        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"catalog entry '{check_id}' has an empty evidence field name")
        if name in seen:
            raise ValueError(f"catalog entry '{check_id}' repeats evidence field '{name}'")
        seen.add(name)
        if name in PREFLIGHT_FINDING_FIELDS:
            continue
        if name.startswith("evidence.") and len(name) > len("evidence."):
            continue
        raise ValueError(
            f"catalog entry '{check_id}' evidence field '{name}' is neither a report finding "
            "field nor an evidence.<key> entry"
        )


def validate_check_families(check_id: str, families: Any, known_families: set[str]) -> None:
    if not isinstance(families, list) or not families:
        raise ValueError(f"catalog entry '{check_id}' needs at least one family")
    unknown = sorted(family for family in families if family not in known_families)
    if unknown:
        raise ValueError(f"catalog entry '{check_id}' names unknown families: {', '.join(unknown)}")
    if len(set(families)) != len(families):
        raise ValueError(f"catalog entry '{check_id}' repeats a family")


def validate_check_fixups(check_id: str, fixups: Any, known_fixups: set[str]) -> None:
    if not isinstance(fixups, list):
        raise ValueError(f"catalog entry '{check_id}' fixups must be an array")
    seen: set[str] = set()
    for fixup_id in fixups:
        if not isinstance(fixup_id, str) or not fixup_id:
            raise ValueError(f"catalog entry '{check_id}' has an empty fixup id")
        if fixup_id in seen:
            raise ValueError(f"catalog entry '{check_id}' repeats fixup '{fixup_id}'")
        seen.add(fixup_id)
        if fixup_id not in known_fixups:
            raise ValueError(
                f"catalog entry '{check_id}' fixup '{fixup_id}' is not a registered preflight fixup"
            )


def build_preflight_check_catalog(registry: list[str], fixup_registry: list[dict[str, Any]]) -> dict[str, Any]:
    overlay = json.loads(read(PREFLIGHT_OVERLAY_PATH))
    overlay_ids = unique_sorted(overlay.get("checks", {}).keys())
    missing = sorted(set(registry) - set(overlay_ids))
    extra = sorted(set(overlay_ids) - set(registry))
    if missing or extra:
        problems = []
        if missing:
            problems.append("registered without catalog: " + ", ".join(missing))
        if extra:
            problems.append("catalog without registry: " + ", ".join(extra))
        raise ValueError("; ".join(problems))
    required = {"measures", "limitations", "coverage", "families", "parameters", "severity", "evidence", "fixups"}
    known_fields = parse_profile_check_field_names()
    known_families = set(overlay["gwg_families"])
    known_fixups = set(preflight_fixup_ids(fixup_registry))
    for check_id, entry in overlay["checks"].items():
        absent = sorted(required - set(entry))
        if absent:
            raise ValueError(f"catalog entry '{check_id}' missing {', '.join(absent)}")
        if entry["coverage"] not in {"covered", "partial", "not_covered"}:
            raise ValueError(f"catalog entry '{check_id}' has invalid coverage")
        validate_check_families(check_id, entry["families"], known_families)
        validate_check_parameters(check_id, entry["parameters"], known_fields)
        validate_check_severity(check_id, entry["severity"])
        validate_check_evidence(check_id, entry["evidence"])
        validate_check_fixups(check_id, entry["fixups"], known_fixups)
    unassigned_fixups = sorted(
        fixup_id
        for fixup_id in known_fixups
        if not any(fixup_id in entry["fixups"] for entry in overlay["checks"].values())
    )
    if unassigned_fixups:
        raise ValueError(
            "registered preflight fixups that no catalog entry claims: " + ", ".join(unassigned_fixups)
        )
    return {
        "format_version": 1,
        "generated_by": "scripts/generate-architecture-catalogs.py",
        "claim": overlay["claim"],
        "matrix_id": overlay["matrix_id"],
        "gwg_families": overlay["gwg_families"],
        "pdfx_targets": overlay["pdfx_targets"],
        "checks": overlay["checks"],
        "not_covered": overlay["not_covered"],
        "registry": registry,
        "fixup_registry": sorted(known_fixups),
    }


def build_preflight_backlog(
    overlay: dict[str, Any],
    registry: list[str],
    fixup_registry: list[dict[str, Any]],
) -> dict[str, Any]:
    """Validate the prioritised coverage backlog and emit it as its own artifact.

    Every gap row is cross-referenced: ``closed_by`` is the literal ``unfiled``,
    a GitHub issue number the overlay recorded from ``gh issue view``, or a
    registered check id. Nothing here is inferred -- an unverified issue number
    fails the build, and a state that disagrees with the recorded issue state
    fails the build, so the backlog cannot rot into a wish list.
    """
    issues = overlay.get("github_issues")
    if not isinstance(issues, dict) or not issues:
        raise ValueError("preflight catalog overlay is missing github_issues")
    verified: dict[str, dict[str, Any]] = {}
    for reference, entry in issues.items():
        if not isinstance(reference, str) or not GITHUB_ISSUE_REF.fullmatch(reference):
            raise ValueError(f"github_issues key '{reference}' is not a '#<number>' reference")
        if not isinstance(entry, dict):
            raise ValueError(f"github_issues entry '{reference}' must be an object")
        absent = sorted({"number", "title", "state", "milestone"} - set(entry))
        if absent:
            raise ValueError(f"github_issues entry '{reference}' missing {', '.join(absent)}")
        if entry["number"] != int(reference[1:]):
            raise ValueError(f"github_issues entry '{reference}' records a different number")
        if entry["state"] not in {"OPEN", "CLOSED"}:
            raise ValueError(f"github_issues entry '{reference}' must record the GitHub state verbatim")
        if not isinstance(entry["title"], str) or not entry["title"].strip():
            raise ValueError(f"github_issues entry '{reference}' needs a title")
        verified[reference] = entry

    rows = overlay.get("backlog")
    if not isinstance(rows, list) or not rows:
        raise ValueError("preflight coverage backlog is empty")
    known_families = set(overlay["gwg_families"])
    known_checks = set(registry)
    not_covered = overlay["not_covered"]
    seen_ids: set[str] = set()
    parsed: list[dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("preflight coverage backlog rows must be objects")
        absent = sorted(set(PREFLIGHT_BACKLOG_FIELDS) - set(row))
        extra = sorted(set(row) - set(PREFLIGHT_BACKLOG_FIELDS))
        if absent or extra:
            raise ValueError(
                "preflight coverage backlog row must carry exactly " + ", ".join(PREFLIGHT_BACKLOG_FIELDS)
            )
        identifier = row["id"]
        if not isinstance(identifier, str) or not CHECK_ID.fullmatch(identifier):
            raise ValueError(f"preflight coverage backlog row id '{identifier}' is invalid")
        if identifier in seen_ids:
            raise ValueError(f"preflight coverage backlog has a duplicate id: {identifier}")
        seen_ids.add(identifier)
        if row["priority"] not in PREFLIGHT_BACKLOG_PRIORITIES:
            raise ValueError(f"backlog row '{identifier}' has invalid priority '{row['priority']}'")
        state = row["state"]
        if state not in PREFLIGHT_BACKLOG_STATES:
            raise ValueError(f"backlog row '{identifier}' has invalid state '{state}'")
        gap = row["gap"]
        if not isinstance(gap, str) or not gap.strip():
            raise ValueError(f"backlog row '{identifier}' needs a gap description")
        families = row["families"]
        if not isinstance(families, list) or not families:
            raise ValueError(f"backlog row '{identifier}' needs at least one family")
        unknown = sorted(family for family in families if family not in known_families)
        if unknown:
            raise ValueError(f"backlog row '{identifier}' names unknown families: {', '.join(unknown)}")
        if len(set(families)) != len(families):
            raise ValueError(f"backlog row '{identifier}' repeats a family")
        closed_by = row["closed_by"]
        if closed_by == PREFLIGHT_BACKLOG_UNFILED:
            if state != "open":
                raise ValueError(f"backlog row '{identifier}' is unfiled but its state is '{state}'")
        elif isinstance(closed_by, str) and GITHUB_ISSUE_REF.fullmatch(closed_by):
            if closed_by not in verified:
                raise ValueError(
                    f"backlog row '{identifier}' references unverified issue {closed_by}; "
                    "confirm it with 'gh issue view' and record it in github_issues first"
                )
            if state == "landed":
                raise ValueError(f"backlog row '{identifier}' is landed by a check, not by {closed_by}")
            expected = "open" if verified[closed_by]["state"] == "OPEN" else "closed"
            if state != expected:
                raise ValueError(
                    f"backlog row '{identifier}' state '{state}' disagrees with {closed_by} "
                    f"({verified[closed_by]['state']})"
                )
        elif isinstance(closed_by, str) and closed_by in known_checks:
            if state != "landed":
                raise ValueError(f"backlog row '{identifier}' names check '{closed_by}' but is not landed")
        else:
            raise ValueError(
                f"backlog row '{identifier}' closed_by must be '{PREFLIGHT_BACKLOG_UNFILED}', "
                "a verified '#<issue>' or a registered check id"
            )
        parsed.append(
            {
                "id": identifier,
                "priority": row["priority"],
                "gap": gap,
                "families": families,
                "state": state,
                "closed_by": closed_by,
            }
        )

    for uncovered in not_covered:
        if not any(row["priority"] == "P1" and uncovered in row["gap"] for row in parsed):
            raise ValueError(f"uncovered class is missing from the backlog: {uncovered}")
    for row in parsed:
        if row["priority"] == "P1" and not any(uncovered in row["gap"] for uncovered in not_covered):
            raise ValueError(f"P1 backlog row '{row['id']}' does not name a not_covered class")

    referenced: set[str] = set()
    for row in parsed:
        if GITHUB_ISSUE_REF.fullmatch(row["closed_by"]):
            referenced.add(row["closed_by"])
        referenced.update(re.findall(GITHUB_ISSUE_REF, row["gap"]))
    unused = sorted(reference for reference in verified if reference not in referenced)
    if unused:
        raise ValueError("github_issues entries cited by no backlog row: " + ", ".join(unused))
    unverified = sorted(reference for reference in referenced if reference not in verified)
    if unverified:
        raise ValueError("backlog rows cite issues with no verified record: " + ", ".join(unverified))

    priority_rule = overlay.get("backlog_priority_rule")
    state_rule = overlay.get("backlog_state_rule")
    for name, rule in (("backlog_priority_rule", priority_rule), ("backlog_state_rule", state_rule)):
        if not isinstance(rule, str) or not rule.strip():
            raise ValueError(f"preflight catalog overlay needs a non-empty {name}")

    parsed.sort(key=lambda row: (row["priority"], row["id"]))
    return {
        "format_version": 1,
        "generated_by": "scripts/generate-architecture-catalogs.py",
        "claim": overlay["claim"],
        "matrix_id": overlay["matrix_id"],
        "priority_rule": priority_rule,
        "state_rule": state_rule,
        "families": overlay["gwg_families"],
        "fixup_registry": preflight_fixup_ids(fixup_registry),
        "not_covered": not_covered,
        "github_issues": verified,
        "rows": parsed,
    }



SAVE_POLICY_MODES = {
    "incrementalAppend": "incremental-append",
    "fullRewrite": "full-rewrite",
    "saveAsNewArtifact": "save-as-new-artifact",
}


def parse_bool_assignment(body: str, name: str, default: bool) -> bool:
    match = re.search(rf"\b{name}\s*=\s*(true|false)\b", body)
    if not match:
        return default
    return match.group(1) == "true"


def parse_evidence_domains(body: str) -> list[str]:
    domains: list[str] = []
    for domain in ("Images", "Colorants", "Strokes", "OverprintTransparency", "Fonts"):
        if re.search(rf"PDFEvidenceDomain::\s*{domain}\b", body):
            domains.append(domain[0].lower() + domain[1:])
    combined = re.search(r"declared\.domains\s*=\s*PDFEvidenceDomains\(([^)]+)\)", body)
    if combined:
        for domain in ("Images", "Colorants", "Strokes", "OverprintTransparency", "Fonts"):
            if domain in combined.group(1):
                value = domain[0].lower() + domain[1:]
                if value not in domains:
                    domains.append(value)
    return unique_sorted(domains)


def parse_operation_impact(class_body: str) -> dict[str, Any]:
    match = re.search(
        r"PDFOperationImpact\s+impact\s*\([^)]*\)\s*const\s+override\s*\{(?P<body>.*?)\n\s*\}",
        class_body,
        re.DOTALL,
    )
    if not match:
        return {
            "domains": [],
            "document_wide": True,
            "full_rewrite": True,
            "impact_complete": False,
            "requires_independent_oracle": False,
        }
    body = match.group("body")
    return {
        "domains": parse_evidence_domains(body),
        "all_pages": parse_bool_assignment(body, "allPages", False),
        "document_wide": parse_bool_assignment(body, "documentWide", False),
        "full_rewrite": parse_bool_assignment(body, "full_rewrite", False)
        if "full_rewrite" in body
        else parse_bool_assignment(body, "fullRewrite", False),
        "impact_complete": parse_bool_assignment(body, "impactComplete", False),
        "requires_independent_oracle": parse_bool_assignment(body, "requiresIndependentOracle", False),
    }


def revalidation_class(impact: dict[str, Any], requires_postflight: bool = True) -> str:
    if not requires_postflight:
        return "none"
    if impact.get("requires_independent_oracle"):
        return "full-with-oracle"
    if not impact.get("impact_complete"):
        return "full"
    if impact.get("document_wide") or not impact.get("domains"):
        return "full"
    return "targeted"


def parse_save_policy(class_body: str) -> dict[str, str | bool]:
    match = re.search(
        r"PDFOperationSavePolicy\s+savePolicy\(\)\s*const\s+override\s*\{(?P<body>.*?)\n\s*\}",
        class_body,
        re.DOTALL,
    )
    if match:
        body = match.group("body")
        save_match = re.search(
            r"PDFOperationSavePolicy::(incrementalAppend|fullRewrite|saveAsNewArtifact)\(",
            body,
        )
        if save_match:
            save_mode = SAVE_POLICY_MODES[save_match.group(1)]
        else:
            save_mode = "save-as-new-artifact"
    else:
        save_mode = "save-as-new-artifact"
    invalidates_signatures = save_mode in {"full-rewrite", "save-as-new-artifact"}
    reversible_in_session = save_mode != "full-rewrite"
    if save_mode == "incremental-append":
        invalidates_signatures = False
    return {
        "mode": save_mode,
        "invalidates_signatures": invalidates_signatures,
        "reversible_in_session": reversible_in_session,
    }


def parse_repair_operation_class(class_body: str, implementation: str) -> dict[str, Any]:
    id_match = re.search(
        r'QString\s+id\(\)\s+const\s+override\s*\{\s*return\s+QStringLiteral\("([^"]+)"\)',
        class_body,
    )
    if not id_match:
        raise ValueError(f"repair operation in {implementation} is missing id()")
    version_match = re.search(r"int\s+version\(\)\s+const\s+override\s*\{\s*return\s+(\d+)", class_body)
    save_policy = parse_save_policy(class_body)
    fixup_match = re.search(
        r"bool\s+isPreflightFixup\(\)\s+const\s+override\s*\{\s*return\s+(true|false)",
        class_body,
    )
    requires_postflight = not re.search(r"plan->requiresPostflight\s*=\s*false", class_body)
    impact = parse_operation_impact(class_body)
    return {
        "id": id_match.group(1),
        "version": int(version_match.group(1)) if version_match else 1,
        "implementation": implementation,
        "is_preflight_fixup": fixup_match.group(1) == "true" if fixup_match else False,
        "requires_postflight": requires_postflight,
        "save_policy": save_policy,
        "impact": impact,
        "revalidation_class": revalidation_class(impact, requires_postflight),
    }


REGISTER_OPERATION_PATTERN = re.compile(
    r"PDFRepairRegistry::instance\(\)\.registerOperation\(std::make_unique<(\w+)>\(\)\)"
)


def parse_registered_repair_classes() -> list[str]:
    classes: list[str] = []
    for path in sorted((ROOT / "LoopLibCore" / "sources").glob("*.cpp")):
        source = read(path)
        classes.extend(REGISTER_OPERATION_PATTERN.findall(source))
    return unique_sorted(classes)


def parse_repair_operations() -> list[dict[str, Any]]:
    operations_by_class: dict[str, dict[str, Any]] = {}
    pattern = re.compile(
        r"class\s+(\w+)\s+final\s*:\s*public\s+PDFRepairOperation(?P<body>.*?)(?=^class\s+\w+\s+final\s*:\s*public\s+PDFRepairOperation|\Z)",
        re.DOTALL | re.MULTILINE,
    )
    for path in sorted((ROOT / "LoopLibCore" / "sources").glob("*.cpp")):
        source = read(path)
        implementation = path.relative_to(ROOT).as_posix()
        for match in pattern.finditer(source):
            class_name = match.group(1)
            operation = parse_repair_operation_class(match.group("body"), implementation)
            operation["class_name"] = class_name
            operations_by_class[class_name] = operation

    registered_classes = parse_registered_repair_classes()
    if not registered_classes:
        raise ValueError("registered operation catalog is empty")

    missing_impl = sorted(set(registered_classes) - set(operations_by_class))
    if missing_impl:
        raise ValueError(
            "registerOperation targets without parsable PDFRepairOperation class: "
            + ", ".join(missing_impl)
        )

    unregistered = sorted(set(operations_by_class) - set(registered_classes))
    if unregistered:
        raise ValueError(
            "PDFRepairOperation classes without registerOperation: " + ", ".join(unregistered)
        )

    operations = [operations_by_class[class_name] for class_name in registered_classes]
    operations.sort(key=lambda operation: operation["id"])
    if len({operation["id"] for operation in operations}) != len(operations):
        raise ValueError("registered operation catalog contains duplicate ids")
    return operations


def build_correction_operation_catalog(registry: list[dict[str, Any]]) -> dict[str, Any]:
    overlay = json.loads(read(CORRECTION_OVERLAY_PATH))
    overlay_ids = unique_sorted(overlay.get("operations", {}).keys())
    registry_ids = unique_sorted(operation["id"] for operation in registry)
    missing = sorted(set(registry_ids) - set(overlay_ids))
    extra = sorted(set(overlay_ids) - set(registry_ids))
    if missing or extra:
        problems = []
        if missing:
            problems.append("registered without catalog: " + ", ".join(missing))
        if extra:
            problems.append("catalog without registry: " + ", ".join(extra))
        raise ValueError("; ".join(problems))

    required = {
        "supports",
        "produces",
        "target_scopes",
        "parameter_defaults",
        "save_policy_artifact_effect",
        "reversibility",
        "evidence_impact",
        "revalidation",
        "surface_parity",
        "limitations",
    }
    parity_surfaces = {"cli", "pagemaster", "editor", "action_list"}
    merged: dict[str, Any] = {}
    registry_by_id = {operation["id"]: operation for operation in registry}
    for operation_id, entry in overlay["operations"].items():
        absent = sorted(required - set(entry))
        if absent:
            raise ValueError(f"catalog entry '{operation_id}' missing {', '.join(absent)}")
        parity = entry["surface_parity"]
        if not isinstance(parity, dict) or parity_surfaces - set(parity):
            raise ValueError(f"catalog entry '{operation_id}' has incomplete surface_parity")
        revalidation = entry["revalidation"]
        if not isinstance(revalidation, dict) or "class" not in revalidation or "description" not in revalidation:
            raise ValueError(f"catalog entry '{operation_id}' has invalid revalidation block")
        declared_class = registry_by_id[operation_id]["revalidation_class"]
        if revalidation["class"] != declared_class:
            raise ValueError(
                f"catalog entry '{operation_id}' revalidation.class {revalidation['class']!r} "
                f"does not match registry impact ({declared_class!r})"
            )
        merged[operation_id] = {
            **registry_by_id[operation_id],
            **entry,
            "save_policy": {
                **registry_by_id[operation_id]["save_policy"],
                "artifact_effect": entry["save_policy_artifact_effect"],
            },
        }

    return {
        "format_version": 1,
        "generated_by": "scripts/generate-architecture-catalogs.py",
        "claim": overlay["claim"],
        "matrix_id": overlay["matrix_id"],
        "save_modes": overlay["save_modes"],
        "target_scope_model": overlay["target_scope_model"],
        "operations": merged,
        "registry": registry_ids,
    }


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
    source = read(ROOT / "LoopLibCore" / "sources" / "pdfschemaversion.cpp")
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
    schema_dir = ROOT / "loop-preflight" / "schemas"
    for path in sorted(schema_dir.glob("*.json")):
        document = json.loads(read(path))
        versions = unique_sorted(str(value) for value in schema_version_values(document))
        schemas[path.relative_to(ROOT).as_posix()] = [int(value) for value in versions]

    engine_header = read(ROOT / "LoopLibCore" / "sources" / "preflightengine.h")
    report_match = re.search(r"PREFLIGHT_REPORT_SCHEMA_VERSION\s*=\s*(\d+)", engine_header)
    engine_cpp = read(ROOT / "LoopLibCore" / "sources" / "preflightengine.cpp")
    decision_match = re.search(
        r"preflightDecisionsToJson.*?PDFSchemaKind::PreflightDecisions,\s*PDFSchemaVersion\{\s*(\d+)\s*,", engine_cpp, re.DOTALL
    )
    action_list = read(ROOT / "LoopLibCore" / "sources" / "pdfactionlist.cpp")
    action_match = re.search(r'loop-action-list/(\d+)', action_list)
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
    unit_test_dir = ROOT / "UnitTests"
    cmake_sources = [
        unit_test_dir / "CMakeLists.txt",
        unit_test_dir / "phase4-tests.cmake",
    ]
    source = "\n".join(read(path) for path in cmake_sources)
    targets = re.findall(r"add_executable\(\s*(UnitTests[A-Za-z0-9_]*)\b", source)
    targets = unique_sorted(targets)
    if not targets:
        raise ValueError("CMake test target catalog is empty")
    return targets


def parse_architecture_invariants(test_targets: list[str] | None = None) -> list[dict[str, Any]]:
    document = json.loads(read(INVARIANTS_PATH))
    invariants = document.get("invariants")
    if not isinstance(invariants, list) or len(invariants) < 20:
        raise ValueError("architecture invariants must list at least 20 numbered entries")
    known_targets = set(test_targets if test_targets is not None else parse_test_targets())
    seen_ids: set[str] = set()
    parsed: list[dict[str, Any]] = []
    for entry in invariants:
        if not isinstance(entry, dict):
            raise ValueError("architecture invariant entries must be objects")
        identifier = entry.get("id")
        title = entry.get("title")
        mapped = entry.get("test_targets")
        if not isinstance(identifier, str) or not re.fullmatch(r"I\d{2}", identifier):
            raise ValueError(f"invalid architecture invariant id: {identifier!r}")
        if identifier in seen_ids:
            raise ValueError(f"duplicate architecture invariant id: {identifier}")
        if not isinstance(title, str) or not title.strip():
            raise ValueError(f"{identifier} is missing a title")
        if not isinstance(mapped, list) or not mapped:
            raise ValueError(f"{identifier} must map to at least one test target")
        unknown = sorted(target for target in mapped if target not in known_targets)
        if unknown:
            raise ValueError(f"{identifier} maps to unknown test targets: {', '.join(unknown)}")
        seen_ids.add(identifier)
        parsed.append(
            {
                "id": identifier,
                "title": title.strip(),
                "test_targets": unique_sorted(str(target) for target in mapped),
            }
        )
    return parsed


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
    registry = parse_preflight_checks()
    repair_registry = parse_repair_operations()
    test_targets = parse_test_targets()
    return {
        "format_version": 1,
        "generated_by": "scripts/generate-architecture-catalogs.py",
        "architecture_invariants": parse_architecture_invariants(test_targets),
        "branch_policy": parse_branch_policy(),
        "version_policy": parse_version_policy(),
        "preflight_checks": registry,
        "preflight_check_catalog": "docs/generated/preflight-check-catalog.json",
        "preflight_coverage_backlog": "docs/generated/preflight-coverage-backlog.json",
        "registered_operations": [
            {"id": operation["id"], "implementation": operation["implementation"]}
            for operation in repair_registry
        ],
        "correction_operation_catalog": "docs/generated/correction-operation-catalog.json",
        "schema_versions": parse_schema_versions(),
        "schema_kinds": parse_schema_kinds(),
        "preflight_coverage": parse_coverage_matrix(),
        "test_targets": test_targets,
        "workflow_branches": parse_workflow_branches(),
        "sources": [
            "docs/branch-policy.json",
            "docs/version-policy.json",
            "docs/architecture-invariants.json",
            "LoopLibCore/sources/preflightengine.cpp",
            "LoopLibCore/sources/preflightengine.h",
            "docs/preflight-check-catalog-overlay.json",
            "docs/correction-operation-catalog-overlay.json",
            "LoopLibCore/sources/pdfactionlist.cpp",
            "LoopLibCore/sources/pdfrepairoperation.cpp",
            "LoopLibCore/sources/pdfrepairprimitives.cpp",
            "LoopLibCore/sources/pdfproductionrepair.cpp",
            "loop-preflight/schemas/*.json",
            "LoopLibCore/sources/pdfschemaversion.cpp",
            "docs/PDFX_POLICY_MATRIX.md",
            "UnitTests/CMakeLists.txt",
            ".github/workflows/*.yml",
        ],
    }


def serialized_catalog() -> str:
    return json.dumps(build_catalog(), indent=2, sort_keys=True) + "\n"


def serialized_preflight_catalog() -> str:
    registry = parse_preflight_checks()
    return json.dumps(build_preflight_check_catalog(registry, parse_repair_operations()), indent=2, sort_keys=True) + "\n"


def serialized_preflight_backlog() -> str:
    overlay = json.loads(read(PREFLIGHT_OVERLAY_PATH))
    registry = parse_preflight_checks()
    backlog = build_preflight_backlog(overlay, registry, parse_repair_operations())
    return json.dumps(backlog, indent=2, sort_keys=True) + "\n"


def serialized_correction_catalog() -> str:
    registry = parse_repair_operations()
    return json.dumps(build_correction_operation_catalog(registry), indent=2, sort_keys=True) + "\n"


def check_generated(path: Path, expected: str, label: str) -> int:
    if not path.exists():
        print(f"error: generated {label} is missing: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1
    actual = read(path)
    if actual == expected:
        return 0
    diff = difflib.unified_diff(
        actual.splitlines(),
        expected.splitlines(),
        fromfile=str(path.relative_to(ROOT)),
        tofile="generated output",
        lineterm="",
    )
    print(f"generated {label} is stale:", file=sys.stderr)
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
        expected_preflight = serialized_preflight_catalog()
        expected_backlog = serialized_preflight_backlog()
        expected_correction = serialized_correction_catalog()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: cannot generate architecture catalog: {error}", file=sys.stderr)
        return 1

    if args.write:
        CATALOG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CATALOG_PATH.write_text(expected, encoding="utf-8", newline="\n")
        PREFLIGHT_CATALOG_PATH.write_text(expected_preflight, encoding="utf-8", newline="\n")
        PREFLIGHT_BACKLOG_PATH.write_text(expected_backlog, encoding="utf-8", newline="\n")
        CORRECTION_CATALOG_PATH.write_text(expected_correction, encoding="utf-8", newline="\n")
        return 0
    return (
        check_generated(CATALOG_PATH, expected, "architecture catalog")
        or check_generated(PREFLIGHT_CATALOG_PATH, expected_preflight, "preflight check catalog")
        or check_generated(PREFLIGHT_BACKLOG_PATH, expected_backlog, "preflight coverage backlog")
        or check_generated(CORRECTION_CATALOG_PATH, expected_correction, "correction operation catalog")
    )


if __name__ == "__main__":
    raise SystemExit(main())
