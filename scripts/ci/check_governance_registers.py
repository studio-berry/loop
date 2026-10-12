#!/usr/bin/env python3
"""Validate G00 ownership records and regenerate their reference tables."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.agent.yaml_subset import load_yaml

REGISTER_DIR = "docs/governance"
MODULE_IDS = {f"L{number:02d}" for number in range(1, 15)} | {"R00", "X00"}
DISPOSITIONS = {"partially-present", "gap", "duplicate", "obsolete", "deferred"}
SOURCE_TABLE = "docs/GOVERNANCE_SOURCE_REGISTER.md"
PROOF_TABLE = "docs/ARCHITECTURE_PROOF_DISPOSITIONS.md"


def read_registers(root: Path) -> tuple[dict, dict]:
    return tuple(json.loads((root / REGISTER_DIR / name).read_text(encoding="utf-8"))
                 for name in ("source-register.json", "proof-obligations.json"))


def pending_obligations(root: Path) -> dict[str, str]:
    invariants = load_yaml((root / "architecture/invariants.yaml").read_text(encoding="utf-8"))
    budgets = load_yaml((root / "architecture/quality-budgets.yaml").read_text(encoding="utf-8"))
    return {
        **{row["id"]: row["enforcement"] for row in invariants["checks"] if row.get("todo")},
        **{row["id"]: row["enforcement"] for row in budgets["metrics"]
           if row["enforcement"] == "declared"},
    }


def validate_data(root: Path, sources: dict, proofs: dict) -> list[str]:
    errors = []
    for label, document in (("sources", sources), ("proofs", proofs)):
        if document.get("repository") != "studio-berry/loop":
            errors.append(f"{label}: repository must be studio-berry/loop")
        if not re.fullmatch(r"[0-9a-f]{40}", str(document.get("reviewed_sha", ""))):
            errors.append(f"{label}: reviewed_sha must be a full SHA")
        if not document.get("reviewed_on"):
            errors.append(f"{label}: review date is absent")
    if sources.get("reviewed_sha") != proofs.get("reviewed_sha"):
        errors.append("registers have different review SHAs")
    modules = sources.get("modules", [])
    module_ids = [row["id"] for row in modules]
    if set(module_ids) != MODULE_IDS or len(module_ids) != len(MODULE_IDS):
        errors.append("source register must cover L01-L14, R00 and X00 exactly once")
    expected = pending_obligations(root)
    obligations = proofs.get("obligations", [])
    ids = [row["id"] for row in obligations]
    if set(ids) != set(expected) or len(ids) != len(expected):
        errors.append("proof register must cover every pending invariant and quality metric exactly once")
    for row in modules + obligations + sources.get("special_dispositions", []):
        for field in ("owner", "disposition", "evidence", "next_action", "limitation", "issues"):
            if not row.get(field):
                errors.append(f"{row['id']}: missing {field}")
        for path in row.get("evidence", []):
            target = (root / path).resolve()
            if not target.is_relative_to(root.resolve()) or not target.is_file():
                errors.append(f"{row['id']}: evidence does not resolve: {path}")
        for number in row.get("issues", []):
            if str(number) not in sources.get("issues", {}):
                errors.append(f"{row['id']}: issue #{number} has no qualified snapshot")
    for row in modules:
        if row["disposition"] not in DISPOSITIONS:
            errors.append(f"{row['id']}: invalid source disposition")
        if not row.get("inputs") or not row.get("source_availability"):
            errors.append(f"{row['id']}: source identity or availability is absent")
        for source in row.get("inputs", []):
            if not re.fullmatch(r"legacy#[0-9]+|notion:[0-9a-f]{32}|studio-berry/loop#[0-9]+", source):
                errors.append(f"{row['id']}: unqualified input {source}")
    for row in obligations:
        if row.get("enforcement") != expected.get(row["id"]):
            errors.append(f"{row['id']}: enforcement disagrees with the architecture contract")
        if row.get("disposition") not in {"deferred", "fulfilled"}:
            errors.append(f"{row['id']}: pending proof needs a fulfillment or explicit deferral")
        if row.get("measured_baseline") is not None:
            errors.append(f"{row['id']}: this inventory does not admit new measurement baselines")
    for number, issue in sources.get("issues", {}).items():
        if issue.get("repository") != sources.get("repository") or str(issue.get("number")) != number:
            errors.append(f"issue #{number}: repository/number identity disagrees")
        if not issue.get("title") or issue.get("state") not in {"OPEN", "CLOSED"}:
            errors.append(f"issue #{number}: title/state snapshot is absent")
    for retired in (".github/workflows/sync-milestones.yml", "docs/github-milestones/manifest.json"):
        if (root / retired).exists():
            errors.append(f"{retired}: retired milestone write input was restored")
    surface = json.loads((root / "docs/product-surface.json").read_text(encoding="utf-8"))
    actual = {row["id"]: row["follow_up_issue"] for row in surface["surfaces"]
              if row["follow_up_issue"] is not None}
    planned = {identifier: row["issue"] for identifier, row in sources["product_follow_ups"].items()}
    if actual != planned:
        errors.append("product follow-up identities disagree with the reviewed source register")
    for number in planned.values():
        if str(number) not in sources["issues"]:
            errors.append(f"product follow-up #{number} has no qualified issue snapshot")
    return errors


def tracker_findings(snapshot: dict, live: dict[int, dict]) -> list[str]:
    errors = []
    for number, expected in snapshot.items():
        current = live.get(int(number))
        if current is None:
            errors.append(f"issue #{number}: not an issue in studio-berry/loop")
            continue
        milestone = (current.get("milestone") or {}).get("title")
        if any(current[field] != expected[field] for field in ("title", "state")) or milestone != expected["milestone"]:
            errors.append(f"issue #{number}: live title/state/milestone disagrees with the frozen snapshot")
    return errors


def github_findings(sources: dict) -> list[str]:
    command = ["gh", "issue", "list", "--repo", sources["repository"], "--state", "all",
               "--limit", "1000", "--json", "number,title,state,milestone"]
    result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8")
    live = {row["number"]: row for row in json.loads(result.stdout)}
    errors = tracker_findings(sources["issues"], live)
    for expected in sources.get("pull_requests", []):
        result = subprocess.run(
            ["gh", "pr", "view", str(expected["number"]), "--repo", sources["repository"],
             "--json", "number,title,state,mergeCommit,headRefOid"],
            check=True, capture_output=True, text=True, encoding="utf-8")
        current = json.loads(result.stdout)
        sha = (current.get("mergeCommit") or {}).get("oid") or current.get("headRefOid")
        if current["title"] != expected["title"] or current["state"] != expected["state"] or sha != expected["sha"]:
            errors.append(f"PR #{expected['number']}: live identity disagrees with the frozen snapshot")
    return errors


def issue_links(numbers: list[int]) -> str:
    return ", ".join(f"[#{number}](https://github.com/studio-berry/loop/issues/{number})"
                     for number in numbers)


def evidence_links(paths: list[str]) -> str:
    return ", ".join(f"[{path}](../{path})" for path in paths)


def tables(sources: dict, proofs: dict) -> dict[str, str]:
    introduction = (
        f"Reviewed source: `{sources['reviewed_sha']}`. Read-back date: {sources['reviewed_on']}.\n\n"
        "Generated from the JSON records under [governance](governance/). "
        "Regenerate with `python scripts/ci/check_governance_registers.py --write`.\n\n"
        "These records identify source, code, tests, owners, and decisions. "
        "A closed issue or merged PR supplies tracking provenance. Runtime proof, independent proof, "
        "and release admission are not revalidated by this inventory.\n\n")
    source_lines = ["# Cross-module source and disposition register\n\n", introduction,
                    "## Named roadmap sources\n\n",
                    "| Source | Availability and disposition | Owner and next action |\n",
                    "| --- | --- | --- |\n"]
    for row in sources["sources"]:
        source_lines.append(f"| [{row['title']}]({row['url']}) | {row['availability']} | {row['next_action']} |\n")
    source_lines.extend(["\n## Module intake\n\n",
                         "| Module and source identities | Owner and disposition | Current issues | Evidence | Next action and limits |\n",
                         "| --- | --- | --- | --- | --- |\n"])
    for row in sources["modules"]:
        identities = ", ".join(f"`{identity}`" for identity in row["inputs"])
        source_lines.append(
            f"| {row['id']}: {identities} | {row['owner']}; {row['disposition']} | "
            f"{issue_links(row['issues'])} | {evidence_links(row['evidence'])} | "
            f"{row['next_action']} {row['source_availability']} {row['limitation']} |\n")
    source_lines.extend(["\n## Special historical dispositions\n\n",
                         "| Source | Owner and disposition | Next action and limits |\n",
                         "| --- | --- | --- |\n"])
    for row in sources["special_dispositions"]:
        source_lines.append(f"| `{row['id']}` | {row['owner']}; {row['disposition']}; "
                            f"{issue_links(row['issues'])} | {row['next_action']} {row['limitation']} |\n")
    source_lines.extend(["\n## Product follow-up reconciliation\n\n",
                         "| Product record | Retired reference | Current owner |\n",
                         "| --- | --- | --- |\n"])
    for identifier, row in sources["product_follow_ups"].items():
        source_lines.append(f"| `{identifier}` | legacy #{row['legacy']} | {issue_links([row['issue']])} |\n")
    source_lines.extend(["\n## Live tracker snapshot\n\n",
                         "| Qualified issue | Title | State at review | Milestone |\n",
                         "| --- | --- | --- | --- |\n"])
    for number, row in sorted(sources["issues"].items(), key=lambda item: int(item[0])):
        source_lines.append(
            f"| [studio-berry/loop#{number}](https://github.com/studio-berry/loop/issues/{number}) | "
            f"{row['title']} | {row['state']} | {row['milestone'] or 'None'} |\n")
    source_lines.extend(["\n## Existing pull-request provenance\n\n",
                         "| PR | Title and status | Merge or head SHA |\n", "| --- | --- | --- |\n"])
    for row in sources["pull_requests"]:
        source_lines.append(f"| [#{row['number']}](https://github.com/studio-berry/loop/pull/{row['number']}) | "
                            f"{row['title']}; {row['state']} | `{row['sha']}` |\n")
    proof_lines = ["# Architecture proof ownership and dispositions\n\n", introduction,
                   "No new metric enforcement is admitted. Fixed budgets and sealed outputs are unchanged. "
                   "A supplemental guard that requires an exported API or schema remains subject to an architect checkpoint.\n\n",
                   "| Obligation | Current declaration and disposition | Owner and tracker | Existing evidence | Closure criterion and limits |\n",
                   "| --- | --- | --- | --- | --- |\n"]
    for row in proofs["obligations"]:
        proof_lines.append(
            f"| `{row['id']}` | {row['enforcement']}; {row['disposition']} | {row['owner']}; "
            f"{issue_links(row['issues'])} | {evidence_links(row['evidence'])} | "
            f"{row['next_action']} {row['limitation']} |\n")
    return {SOURCE_TABLE: "".join(source_lines), PROOF_TABLE: "".join(proof_lines)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="regenerate the reference tables")
    parser.add_argument("--verify-github", action="store_true", help="read back frozen issue and PR identities")
    args = parser.parse_args()
    try:
        sources, proofs = read_registers(ROOT)
        errors = validate_data(ROOT, sources, proofs)
        if errors:
            raise ValueError("\n".join(errors))
        if args.verify_github:
            errors.extend(github_findings(sources))
        if errors:
            raise ValueError("\n".join(errors))
        for relative, content in tables(sources, proofs).items():
            path = ROOT / relative
            if args.write:
                path.write_text(content, encoding="utf-8", newline="\n")
            elif not path.is_file() or path.read_text(encoding="utf-8") != content:
                errors.append(f"{relative}: stale; run --write")
        if errors:
            raise ValueError("\n".join(errors))
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"governance registers FAILED: {exc}", file=sys.stderr)
        return 1
    print(f"governance registers passed: {len(sources['modules'])} modules, {len(proofs['obligations'])} proof obligations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
