#!/usr/bin/env python3
"""Generate agent instruction adapters from the canonical agent policy."""

from __future__ import annotations

import argparse
import difflib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = ROOT / "agent-policy.json"


def load_policy() -> dict:
    with POLICY_PATH.open(encoding="utf-8") as stream:
        policy = json.load(stream)
    required = {"branches", "autonomy", "module_boundaries", "global_checks", "changelog", "adapters"}
    missing = sorted(required - policy.keys())
    if missing:
        raise ValueError(f"agent-policy.json missing: {', '.join(missing)}")
    if policy.get("format_version") != 1:
        raise ValueError("unsupported agent policy format_version")
    for section in ("branches", "autonomy", "changelog"):
        if not isinstance(policy.get(section), dict):
            raise ValueError(f"agent policy section must be an object: {section}")
    for key in ("default", "integration", "release", "topic_source", "protected"):
        if key not in policy["branches"]:
            raise ValueError(f"agent policy branches missing: {key}")
    if not isinstance(policy["branches"].get("topic_branch_patterns"), list):
        raise ValueError("agent policy branches.topic_branch_patterns must be an array")
    for key in ("allowed", "approval_required"):
        if not isinstance(policy["autonomy"].get(key), list):
            raise ValueError(f"agent policy autonomy.{key} must be an array")
    if not isinstance(policy["module_boundaries"], dict) or not policy["module_boundaries"]:
        raise ValueError("agent policy module_boundaries must be a non-empty object")
    for module, definition in policy["module_boundaries"].items():
        if not all(isinstance(definition.get(key), list) for key in ("paths", "targets", "tests")):
            raise ValueError(f"module {module} must define paths, targets, and tests arrays")
    if not isinstance(policy["global_checks"], list) or not policy["global_checks"]:
        raise ValueError("agent policy global_checks must be a non-empty array")
    if policy["changelog"].get("required_per_pr") != 1:
        raise ValueError("agent policy must require exactly one changelog fragment per PR")
    if not all(policy["changelog"].get(key) for key in ("directory", "filename", "categories", "required_fields")):
        raise ValueError("agent policy changelog section is incomplete")
    if not isinstance(policy["adapters"], list) or not policy["adapters"]:
        raise ValueError("agent policy adapters must be a non-empty array")
    return policy


def render(policy: dict, adapter: str) -> str:
    if adapter == "docs/branch-policy.json":
        branches = policy["branches"]
        return json.dumps(
            {
                "generated_by": "scripts/agent/generate-adapters.py",
                "default_branch": branches["default"],
                "release_branch": branches["release"],
                "integration_branch": branches["integration"],
                "topic_branch_source": branches["topic_source"],
                "topic_branch_patterns": branches["topic_branch_patterns"],
                "protected_branches": branches["protected"]
            },
            indent=2,
        ) + "\n"
    branches = policy["branches"]
    autonomy = policy["autonomy"]
    changelog = policy["changelog"]
    lines = [
        "<!-- GENERATED FILE: edit agent-policy.json and run scripts/agent/generate-adapters.py --write -->",
        "# Loupe agent policy adapter",
        "",
        f"Repository: `{policy.get('repository', 'studio-berry/loupe')}`; language: `{policy.get('language', 'C++20')}`; minimum Qt: `{policy.get('qt_minimum', 'source-defined')}`.",
        "",
        "## Branches and safety",
        "",
        f"- Integration: `{branches['integration']}`; release/default: `{branches['release']}`; topic branches start from `{branches['topic_source']}`.",
        f"- Protected branches: {', '.join(f'`{branch}`' for branch in branches['protected'])}. Do not commit, push, merge, force-push, or rewrite history without approval.",
        "- Keep private data, credentials, logs, scratch plans, and build artifacts outside the repository. Do not edit vendored dependencies unless explicitly scoped.",
        "",
        "## Autonomous verification budget",
        "",
        "Allowed without additional approval:",
    ]
    lines.extend(f"- {item.replace('_', ' ')}" for item in autonomy["allowed"])
    lines.extend(["", "Approval is required for:"])
    lines.extend(f"- {item.replace('_', ' ')}" for item in autonomy["approval_required"])
    lines.extend([
        "",
        "## Required proof and changelog",
        "",
        "- After implementation, run `python scripts/agent/check-change.py --base origin/dev` (or the equivalent base SHA). Treat an incomplete result as not proven.",
        f"- Every PR adds exactly one `{changelog['directory']}/<sanitized-head-branch>.md` fragment. Required fields: {', '.join(changelog['required_fields'])}. Categories: {', '.join(changelog['categories'])}.",
        "- Use `internal` for tooling or documentation changes; it still requires a fragment.",
        "- Do not invent a public contract when a protected interface, schema, persistence format, central type, or root build contract must change; stop and report the contract change.",
        "",
        "## Module placement",
        "",
        "- Core PDF logic belongs in `Pdf4QtLibCore`; it must not depend on Widgets.",
        "- Interactive plugins belong in `Pdf4QtEditorPlugins` hosted by the Editor; batch geometry belongs in PageMaster; unattended pipelines belong in PdfTool.",
        "- Consult the generated architecture catalog and current code/tests for dynamic facts; narrative docs are not authoritative when they conflict.",
        "",
        f"Generated adapter: `{adapter}`.",
        "",
    ])
    return "\n".join(lines).rstrip() + "\n"


def check_or_write(write: bool) -> int:
    policy = load_policy()
    failures = []
    for relative in policy["adapters"]:
        path = ROOT / relative
        expected = render(policy, relative)
        actual = path.read_text(encoding="utf-8") if path.exists() else ""
        if write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(expected, encoding="utf-8", newline="\n")
        elif actual != expected:
            diff = "".join(difflib.unified_diff(actual.splitlines(True), expected.splitlines(True), fromfile=str(path), tofile="generated"))
            failures.append(diff or f"{relative} is stale")
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr, end="" if failure.endswith("\n") else "\n")
        return 1
    print("Agent policy and generated adapters are current.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true", help="write generated adapters")
    parser.add_argument("--check", action="store_true", help="validate generated adapters (default)")
    args = parser.parse_args()
    if args.write and args.check:
        parser.error("--write and --check are mutually exclusive")
    try:
        return check_or_write(args.write)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
