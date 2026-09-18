#!/usr/bin/env python3
"""Validate governed publication identity across product-surface reports.

The checker intentionally validates the evidence contract rather than trying
to parse PDF bytes. Each surface must report the same signed-off identity
chain: plan, source, candidate, published bytes, revalidation report, and
effective profile. Use ``--compare-identity`` when several reports describe
the same operation and must agree on the semantic plan identity.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterator


DIGEST = re.compile(r"^[0-9a-f]{64}$")
IDENTITY_FIELDS = (
    "plan_digest",
    "source_sha256",
    "candidate_sha256",
    "published_sha256",
    "revalidation_report_sha256",
    "effective_profile_digest",
)


def records(value: Any, location: str) -> Iterator[tuple[str, dict[str, Any]]]:
    if not isinstance(value, dict):
        return

    governed = value.get("governed")
    if isinstance(governed, dict):
        yield f"{location}.governed", governed

    if all(isinstance(value.get(key), dict) for key in ("approval", "revalidation", "sign_off")):
        yield location, value

    for key in ("data", "manifest"):
        child = value.get(key)
        if isinstance(child, dict):
            yield from records(child, f"{location}.{key}")

    for key in ("outputs", "items"):
        children = value.get(key)
        if isinstance(children, list):
            for index, child in enumerate(children):
                yield from records(child, f"{location}.{key}[{index}]")


def require_digest(container: dict[str, Any], field: str, location: str, errors: list[str]) -> str:
    value = container.get(field)
    if not isinstance(value, str) or not DIGEST.fullmatch(value.lower()):
        errors.append(f"{location}.{field}: expected a lowercase or uppercase SHA-256 digest")
        return ""
    return value.lower()


def validate_governed(governed: dict[str, Any], location: str, allow_uncertified: bool) -> tuple[list[str], tuple[str, ...] | None]:
    errors: list[str] = []
    approval = governed.get("approval")
    revalidation = governed.get("revalidation")
    sign_off = governed.get("sign_off")
    if not isinstance(approval, dict) or not isinstance(revalidation, dict):
        return [f"{location}: approval and revalidation objects are required"], None

    if not isinstance(sign_off, dict):
        if allow_uncertified:
            return [], None
        return [f"{location}: publication is not signed off"], None

    approval_identity = {
        field: require_digest(approval, field, f"{location}.approval", errors)
        for field in ("plan_digest", "source_sha256", "candidate_sha256")
    }
    signoff_identity = {
        field: require_digest(sign_off, field, f"{location}.sign_off", errors)
        for field in IDENTITY_FIELDS
    }
    artifact = require_digest(revalidation, "artifact_sha256", f"{location}.revalidation", errors)
    report = require_digest(revalidation, "report_sha256", f"{location}.revalidation", errors)
    profile = require_digest(revalidation, "effective_profile_digest", f"{location}.revalidation", errors)

    if revalidation.get("bytes_verified") is not True:
        errors.append(f"{location}.revalidation.bytes_verified: must be true")
    if revalidation.get("sign_off_eligible") is not True:
        errors.append(f"{location}.revalidation.sign_off_eligible: must be true")
    verdict = revalidation.get("verdict")
    if not isinstance(verdict, dict) or verdict.get("state") != "pass":
        errors.append(f"{location}.revalidation.verdict.state: must be pass")

    expected = {
        "plan_digest": approval_identity["plan_digest"],
        "source_sha256": approval_identity["source_sha256"],
        "candidate_sha256": approval_identity["candidate_sha256"],
        "published_sha256": artifact,
        "revalidation_report_sha256": report,
        "effective_profile_digest": profile,
    }
    for field, expected_value in expected.items():
        if expected_value and signoff_identity[field] != expected_value:
            errors.append(f"{location}.sign_off.{field}: does not match the bound evidence")

    signoff_approval = sign_off.get("approval")
    if not isinstance(signoff_approval, dict):
        errors.append(f"{location}.sign_off.approval: required")
    else:
        if signoff_approval.get("decision") != "approve":
            errors.append(f"{location}.sign_off.approval.decision: must be approve")
        if not signoff_approval.get("actorId") or not signoff_approval.get("policyId"):
            errors.append(f"{location}.sign_off.approval: actorId and policyId are required")

    if errors:
        return errors, None
    return [], tuple(signoff_identity[field] for field in ("plan_digest", "source_sha256", "effective_profile_digest"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path, help="JSON reports or manifests to validate")
    parser.add_argument("--allow-uncertified", action="store_true", help="allow explicit preflight/not-certified records")
    parser.add_argument("--compare-identity", action="store_true", help="require all signed-off records to share plan/source/profile identity")
    args = parser.parse_args()

    errors: list[str] = []
    identities: list[tuple[str, tuple[str, ...]]] = []
    record_count = 0
    for report_path in args.reports:
        try:
            document = json.loads(report_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            errors.append(f"{report_path}: cannot read JSON: {error}")
            continue
        found = list(records(document, report_path.as_posix()))
        if not found:
            errors.append(f"{report_path}: no governed publication record found")
            continue
        for location, governed in found:
            record_count += 1
            record_errors, identity = validate_governed(governed, location, args.allow_uncertified)
            errors.extend(record_errors)
            if identity is not None:
                identities.append((location, identity))

    if args.compare_identity and identities:
        expected = identities[0][1]
        for location, identity in identities[1:]:
            if identity != expected:
                errors.append(f"{location}: plan/source/profile identity differs from the first signed-off record")

    if errors:
        print("ERROR: governed parity check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    print(f"Governed parity check passed: {record_count} publication record(s) validated.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
