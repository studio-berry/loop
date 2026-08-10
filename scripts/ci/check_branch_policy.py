#!/usr/bin/env python3
"""Keep workflow branch triggers aligned with the documented branch policy."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EVENTS = ("push", "pull_request")
DOCUMENTED_BRANCHES = re.compile(r"^[-*]\s+CI branches:\s*(.+)$", re.MULTILINE)
DOCUMENTED_REQUIRED_CHECK = re.compile(r"^[-*]\s+Required check:\s*`([^`]+)`$", re.MULTILINE)


def _branch_names(value: str) -> tuple[str, ...]:
    """Read branch names from an inline YAML list or a single scalar."""
    value = value.split("#", 1)[0].strip()
    if value.startswith("[") and "]" in value:
        value = value[1 : value.index("]")]
    names = []
    for item in value.split(","):
        item = item.strip().strip("'\"`")
        if item:
            names.append(item)
    return tuple(names)


def parse_documented_policy(text: str) -> tuple[tuple[str, ...], str]:
    """Return the documented CI branches and required status check."""
    branches_match = DOCUMENTED_BRANCHES.search(text)
    check_match = DOCUMENTED_REQUIRED_CHECK.search(text)
    if not branches_match:
        raise ValueError("policy is missing the `CI branches:` declaration")
    if not check_match:
        raise ValueError("policy is missing the `Required check:` declaration")
    branches = _branch_names(branches_match.group(1))
    if not branches:
        raise ValueError("policy declares no CI branches")
    return branches, check_match.group(1)


def parse_workflow_branch_triggers(text: str) -> dict[str, tuple[str, ...]]:
    """Parse push/pull_request branch lists from a workflow's ``on`` block."""
    triggers: dict[str, tuple[str, ...]] = {}
    in_on = False
    event: str | None = None
    in_branches = False
    branches: list[str] = []

    def finish_branches() -> None:
        nonlocal branches, in_branches
        if event is not None and in_branches:
            triggers[event] = tuple(branches)
        branches = []
        in_branches = False

    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        stripped = line.strip()
        if not stripped:
            continue
        indent = len(line) - len(line.lstrip(" "))

        if indent == 0:
            if stripped == "on:" or stripped.startswith("on: "):
                finish_branches()
                in_on = True
                event = None
                continue
            if in_on:
                finish_branches()
                in_on = False
            continue

        if not in_on:
            continue

        if indent == 2 and stripped.endswith(":"):
            finish_branches()
            candidate = stripped[:-1]
            event = candidate if candidate in EVENTS else None
            continue

        if event is None:
            continue

        if indent == 4 and stripped.startswith("branches:"):
            finish_branches()
            in_branches = True
            inline = stripped[len("branches:") :].strip()
            if inline:
                branches.extend(_branch_names(inline))
            continue

        if in_branches and indent >= 6 and stripped.startswith("-"):
            branches.extend(_branch_names(stripped[1:].strip()))
            continue

        if in_branches and indent <= 4:
            finish_branches()

    finish_branches()
    return triggers


def validate_workflow_branches(
    path: Path, text: str, expected_branches: tuple[str, ...]
) -> list[str]:
    """Validate both event trigger lists against the documented policy."""
    expected = tuple(expected_branches)
    triggers = parse_workflow_branch_triggers(text)
    violations: list[str] = []
    for event in EVENTS:
        actual = triggers.get(event)
        if actual != expected:
            violations.append(
                f"{path}: {event} branches {list(actual or ())} do not match "
                f"documented CI branches {list(expected)}"
            )
    return violations


def validate_repository(root: Path = ROOT) -> list[str]:
    """Validate the policy document and the workflows that gate the branches."""
    policy_path = root / "docs" / "BRANCH_POLICY.md"
    try:
        branches, required_check = parse_documented_policy(policy_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"{policy_path}: {exc}"]

    violations: list[str] = []
    if required_check != "ci_ok":
        violations.append(f"{policy_path}: required check must be ci_ok, got {required_check}")

    for relative in (".github/workflows/ci.yml", ".github/workflows/codeql.yml"):
        workflow = root / relative
        try:
            text = workflow.read_text(encoding="utf-8")
        except OSError as exc:
            violations.append(f"{workflow}: {exc}")
            continue
        violations.extend(validate_workflow_branches(workflow, text, branches))
    return violations


def main() -> int:
    violations = validate_repository()
    if violations:
        for violation in violations:
            print(f"ERROR: {violation}", file=sys.stderr)
        return 1
    print("Branch policy passed: ci.yml and codeql.yml gate the documented branches.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
