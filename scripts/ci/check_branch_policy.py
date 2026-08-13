#!/usr/bin/env python3
"""Keep workflow triggers, documented branch policy, and live protection aligned."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
EVENTS = ("push", "pull_request")
GITHUB_ACTIONS_APP_ID = 15368
GITHUB_API = "https://api.github.com"

DOCUMENTED_CI_BRANCHES = re.compile(r"^[-*]\s+CI branches:\s*(.+)$", re.MULTILINE)
DOCUMENTED_PROTECTED_BRANCHES = re.compile(r"^[-*]\s+Protected branches:\s*(.+)$", re.MULTILINE)
DOCUMENTED_REQUIRED_CHECK = re.compile(r"^[-*]\s+Required check:\s*`([^`]+)`$", re.MULTILINE)
DOCUMENTED_REQUIRED_CHECK_APP = re.compile(r"^[-*]\s+Required check app:\s*(.+)$", re.MULTILINE)
DOCUMENTED_RELEASE_GATE_WORKFLOW = re.compile(
    r"^[-*]\s+Release gate workflow:\s*`([^`]+)`$", re.MULTILINE
)
DOCUMENTED_RELEASE_GATE_EVENTS = re.compile(
    r"^[-*]\s+Release gate events:\s*(.+)$", re.MULTILINE
)
DOCUMENTED_RELEASE_GATE_PR_BRANCHES = re.compile(
    r"^[-*]\s+Release gate pull_request branches:\s*(.+)$", re.MULTILINE
)
DOCUMENTED_INTEGRATION_WORKFLOW = re.compile(
    r"^[-*]\s+Integration workflow:\s*`([^`]+)`$", re.MULTILINE
)
DOCUMENTED_INTEGRATION_PR_BRANCHES = re.compile(
    r"^[-*]\s+Integration pull_request branches:\s*(.+)$", re.MULTILINE
)


@dataclass(frozen=True)
class DocumentedPolicy:
    ci_branches: tuple[str, ...]
    protected_branches: tuple[str, ...]
    required_check: str
    required_check_app: str
    release_gate_workflow: str
    release_gate_events: tuple[str, ...]
    release_gate_pull_request_branches: tuple[str, ...]
    integration_workflow: str
    integration_pull_request_branches: tuple[str, ...]


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
    policy = parse_documented_policy_full(text)
    return policy.ci_branches, policy.required_check


def parse_documented_policy_full(text: str) -> DocumentedPolicy:
    """Return the machine-readable branch-policy declarations."""

    def required(pattern: re.Pattern[str], label: str) -> str:
        match = pattern.search(text)
        if not match:
            raise ValueError(f"policy is missing the `{label}` declaration")
        return match.group(1).strip()

    ci_branches = _branch_names(required(DOCUMENTED_CI_BRANCHES, "CI branches:"))
    if not ci_branches:
        raise ValueError("policy declares no CI branches")
    protected = _branch_names(required(DOCUMENTED_PROTECTED_BRANCHES, "Protected branches:"))
    if not protected:
        raise ValueError("policy declares no protected branches")
    required_check = required(DOCUMENTED_REQUIRED_CHECK, "Required check:")
    required_app = required(DOCUMENTED_REQUIRED_CHECK_APP, "Required check app:")
    release_workflow = required(DOCUMENTED_RELEASE_GATE_WORKFLOW, "Release gate workflow:")
    release_events = _branch_names(required(DOCUMENTED_RELEASE_GATE_EVENTS, "Release gate events:"))
    if not release_events:
        raise ValueError("policy declares no release gate events")
    release_pr = _branch_names(
        required(DOCUMENTED_RELEASE_GATE_PR_BRANCHES, "Release gate pull_request branches:")
    )
    if not release_pr:
        raise ValueError("policy declares no release gate pull_request branches")
    integration_workflow = required(DOCUMENTED_INTEGRATION_WORKFLOW, "Integration workflow:")
    integration_pr = _branch_names(
        required(DOCUMENTED_INTEGRATION_PR_BRANCHES, "Integration pull_request branches:")
    )
    if not integration_pr:
        raise ValueError("policy declares no integration pull_request branches")
    return DocumentedPolicy(
        ci_branches=ci_branches,
        protected_branches=protected,
        required_check=required_check,
        required_check_app=required_app,
        release_gate_workflow=release_workflow,
        release_gate_events=release_events,
        release_gate_pull_request_branches=release_pr,
        integration_workflow=integration_workflow,
        integration_pull_request_branches=integration_pr,
    )


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


def parse_on_events(text: str) -> tuple[str, ...]:
    """Return top-level ``on:`` event names from a workflow file."""
    events: list[str] = []
    in_on = False
    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        stripped = line.strip()
        if not stripped:
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent == 0:
            if stripped == "on:" or stripped.startswith("on: "):
                in_on = True
                inline = stripped[3:].strip().lstrip(":")
                if inline:
                    events.extend(_branch_names(inline))
                continue
            if in_on:
                break
            continue
        if in_on and indent == 2 and stripped.endswith(":"):
            events.append(stripped[:-1])
    return tuple(events)


def on_block_has_path_filters(text: str) -> bool:
    """True when the workflow ``on:`` block uses path filters."""
    in_on = False
    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        stripped = line.strip()
        if not stripped:
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent == 0:
            if stripped == "on:" or stripped.startswith("on: "):
                in_on = True
                continue
            if in_on:
                break
            continue
        if in_on and (stripped.startswith("paths:") or stripped.startswith("paths-ignore:")):
            return True
    return False


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


def validate_release_gate_workflow(path: Path, text: str, policy: DocumentedPolicy) -> list[str]:
    """Validate the dedicated stable release-gate workflow."""
    violations: list[str] = []
    events = parse_on_events(text)
    for required in policy.release_gate_events:
        if required not in events:
            violations.append(f"{path}: missing `{required}` trigger")
    if on_block_has_path_filters(text):
        violations.append(f"{path}: release gate must not use path filters")
    triggers = parse_workflow_branch_triggers(text)
    actual_pr = triggers.get("pull_request")
    if actual_pr != policy.release_gate_pull_request_branches:
        violations.append(
            f"{path}: pull_request branches {list(actual_pr or ())} do not match "
            f"documented release gate pull_request branches "
            f"{list(policy.release_gate_pull_request_branches)}"
        )
    if "release_ok:" not in text and "release_ok :" not in text:
        violations.append(f"{path}: missing release_ok job")
    if "if: always()" not in text and "if: ${{ always() }}" not in text:
        violations.append(f"{path}: release_ok must use if: always()")
    return violations


def validate_integration_workflow(path: Path, text: str, policy: DocumentedPolicy) -> list[str]:
    """Validate the nonblocking integration workflow."""
    violations: list[str] = []
    triggers = parse_workflow_branch_triggers(text)
    actual_push = triggers.get("push")
    if actual_push != policy.ci_branches:
        violations.append(
            f"{path}: push branches {list(actual_push or ())} do not match "
            f"documented CI branches {list(policy.ci_branches)}"
        )
    actual_pr = triggers.get("pull_request")
    if actual_pr != policy.integration_pull_request_branches:
        violations.append(
            f"{path}: pull_request branches {list(actual_pr or ())} do not match "
            f"documented integration pull_request branches "
            f"{list(policy.integration_pull_request_branches)}"
        )
    if re.search(r"^\s*ci_ok\s*:", text, re.MULTILINE):
        violations.append(f"{path}: obsolete ci_ok aggregate must not remain")
    return violations


def _required_check_entries(protection: dict[str, Any]) -> list[dict[str, Any]]:
    status = protection.get("required_status_checks")
    if not isinstance(status, dict):
        return []
    checks = status.get("checks")
    if isinstance(checks, list) and checks:
        entries: list[dict[str, Any]] = []
        for item in checks:
            if isinstance(item, dict) and item.get("context"):
                entries.append(item)
        if entries:
            return entries
    contexts = status.get("contexts")
    if isinstance(contexts, list):
        return [{"context": name, "app_id": None} for name in contexts if isinstance(name, str)]
    return []


def validate_live_protection(
    *,
    stable_protection: dict[str, Any] | None,
    dev_protection: dict[str, Any] | None,
    policy: DocumentedPolicy,
) -> list[str]:
    """Compare live GitHub protection JSON with the documented contract."""
    violations: list[str] = []
    if "stable" in policy.protected_branches:
        if not isinstance(stable_protection, dict):
            violations.append("live protection: stable rules were not returned")
        else:
            checks = _required_check_entries(stable_protection)
            contexts = [str(item.get("context")) for item in checks]
            if contexts != [policy.required_check]:
                violations.append(
                    "live protection: stable required checks "
                    f"{contexts} do not match `[{policy.required_check}]`"
                )
            elif checks and policy.required_check_app.lower() == "github actions":
                app_id = checks[0].get("app_id")
                if app_id not in (GITHUB_ACTIONS_APP_ID, str(GITHUB_ACTIONS_APP_ID)):
                    violations.append(
                        "live protection: stable required check "
                        f"`{policy.required_check}` must be bound to GitHub Actions "
                        f"(app_id {GITHUB_ACTIONS_APP_ID}), got {app_id!r}"
                    )
    if isinstance(dev_protection, dict):
        dev_checks = _required_check_entries(dev_protection)
        if dev_checks:
            contexts = [str(item.get("context")) for item in dev_checks]
            violations.append(
                "live protection: dev must not require status checks, "
                f"got {contexts}"
            )
    return violations


def fetch_branch_protection(
    repo: str, branch: str, token: str | None
) -> tuple[dict[str, Any] | None, str | None]:
    """Return protection JSON or an error code of ``403`` / ``404`` / message."""
    request = urllib.request.Request(
        f"{GITHUB_API}/repos/{repo}/branches/{branch}/protection",
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "loupe-check-branch-policy",
            "X-GitHub-Api-Version": "2022-11-28",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return None, str(exc.code)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        return None, str(exc)
    if not isinstance(payload, dict):
        return None, "protection payload is not an object"
    return payload, None


def validate_repository(
    root: Path = ROOT,
    *,
    live: bool = False,
    repo: str | None = None,
    token: str | None = None,
) -> list[str]:
    """Validate the policy document and the workflows that gate the branches."""
    policy_path = root / "docs" / "BRANCH_POLICY.md"
    try:
        policy = parse_documented_policy_full(policy_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"{policy_path}: {exc}"]

    violations: list[str] = []
    if policy.required_check != "release_ok":
        violations.append(
            f"{policy_path}: required check must be release_ok, got {policy.required_check}"
        )
    if policy.required_check_app.lower() != "github actions":
        violations.append(
            f"{policy_path}: required check app must be GitHub Actions, "
            f"got {policy.required_check_app}"
        )

    integration = root / policy.integration_workflow
    try:
        integration_text = integration.read_text(encoding="utf-8")
    except OSError as exc:
        violations.append(f"{integration}: {exc}")
    else:
        violations.extend(validate_integration_workflow(integration, integration_text, policy))

    release_gate = root / policy.release_gate_workflow
    try:
        release_text = release_gate.read_text(encoding="utf-8")
    except OSError as exc:
        violations.append(f"{release_gate}: {exc}")
    else:
        violations.extend(validate_release_gate_workflow(release_gate, release_text, policy))

    codeql = root / ".github/workflows/codeql.yml"
    try:
        codeql_text = codeql.read_text(encoding="utf-8")
    except OSError as exc:
        violations.append(f"{codeql}: {exc}")
    else:
        violations.extend(validate_workflow_branches(codeql, codeql_text, policy.ci_branches))

    if live:
        repo_name = repo or os.environ.get("GITHUB_REPOSITORY")
        auth = token or os.environ.get("LOUPE_POLICY_TOKEN") or os.environ.get(
            "GITHUB_TOKEN"
        ) or os.environ.get("GH_TOKEN")
        if not repo_name or not auth:
            print(
                "WARNING: live branch protection check skipped "
                "(repository or token not available); file-based policy checks still ran.",
                file=sys.stderr,
            )
        else:
            stable, stable_error = fetch_branch_protection(repo_name, "stable", auth)
            dev, dev_error = fetch_branch_protection(repo_name, "dev", auth)
            if stable_error == "403" or dev_error == "403":
                print(
                    "WARNING: live branch protection is not readable with this token; "
                    "file-based policy checks still ran.",
                    file=sys.stderr,
                )
            else:
                if stable_error:
                    violations.append(
                        f"live protection: failed to read stable rules ({stable_error})"
                    )
                if dev_error and dev_error != "404":
                    violations.append(
                        f"live protection: failed to read dev rules ({dev_error})"
                    )
                if not stable_error:
                    violations.extend(
                        validate_live_protection(
                            stable_protection=stable,
                            dev_protection=dev if dev_error != "404" else {},
                            policy=policy,
                        )
                    )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live",
        action="store_true",
        help="Also compare documented policy with live GitHub branch protection.",
    )
    args = parser.parse_args()
    violations = validate_repository(live=args.live)
    if violations:
        for violation in violations:
            print(f"ERROR: {violation}", file=sys.stderr)
        return 1
    print(
        "Branch policy passed: workflow triggers match the documented "
        "dev/stable contract."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
