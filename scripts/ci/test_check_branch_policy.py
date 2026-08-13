#!/usr/bin/env python3
import unittest
from pathlib import Path

from scripts.ci.check_branch_policy import (
    GITHUB_ACTIONS_APP_ID,
    parse_documented_policy,
    parse_documented_policy_full,
    parse_on_events,
    parse_workflow_branch_triggers,
    validate_integration_workflow,
    validate_live_protection,
    validate_release_gate_workflow,
    validate_repository,
    validate_workflow_branches,
)


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_BRANCHES = ("dev", "stable")


class BranchPolicyTests(unittest.TestCase):
    def test_documented_policy_declares_ci_branches_and_required_check(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        branches, required_check = parse_documented_policy(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        self.assertEqual(branches, EXPECTED_BRANCHES)
        self.assertEqual(policy.ci_branches, EXPECTED_BRANCHES)
        self.assertEqual(required_check, "release_ok")
        self.assertEqual(policy.required_check, "release_ok")
        self.assertEqual(policy.required_check_app.lower(), "github actions")
        self.assertEqual(policy.protected_branches, ("stable",))
        self.assertEqual(policy.release_gate_workflow, ".github/workflows/release-gate.yml")
        self.assertEqual(policy.release_gate_events, ("pull_request", "merge_group"))
        self.assertEqual(policy.release_gate_pull_request_branches, ("stable",))
        self.assertEqual(policy.integration_workflow, ".github/workflows/ci.yml")
        self.assertEqual(policy.integration_pull_request_branches, ("dev",))

    def test_current_ci_workflow_matches_policy(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertEqual(parse_workflow_branch_triggers(workflow)["push"], EXPECTED_BRANCHES)
        self.assertEqual(parse_workflow_branch_triggers(workflow)["pull_request"], ("dev",))
        self.assertEqual(validate_integration_workflow(Path("ci.yml"), workflow, policy), [])

    def test_current_release_gate_matches_policy(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        workflow = (ROOT / ".github/workflows/release-gate.yml").read_text(encoding="utf-8")
        self.assertEqual(parse_on_events(workflow)[:2], ("pull_request", "merge_group"))
        self.assertIn("merge_group", parse_on_events(workflow))
        self.assertEqual(
            parse_workflow_branch_triggers(workflow)["pull_request"], ("stable",)
        )
        self.assertEqual(validate_release_gate_workflow(Path("release-gate.yml"), workflow, policy), [])

    def test_current_repository_matches_policy(self):
        self.assertEqual(validate_repository(ROOT), [])

    def test_current_codeql_workflow_matches_policy(self):
        workflow = (ROOT / ".github/workflows/codeql.yml").read_text(encoding="utf-8")
        self.assertEqual(validate_workflow_branches(Path("codeql.yml"), workflow, EXPECTED_BRANCHES), [])

    def test_rejects_deliberately_stale_master_trigger(self):
        stale_workflow = """on:
  push:
    branches: [\"master\"]
  pull_request:
    branches:
      - dev
      - stable
"""
        violations = validate_workflow_branches(Path("stale.yml"), stale_workflow, EXPECTED_BRANCHES)
        self.assertEqual(len(violations), 1)
        self.assertIn("master", violations[0])

    def test_rejects_release_gate_without_merge_group(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        stale = """on:
  pull_request:
    branches: [stable]
  workflow_dispatch:

jobs:
  release_ok:
    if: always()
    runs-on: ubuntu-24.04
"""
        violations = validate_release_gate_workflow(Path("release-gate.yml"), stale, policy)
        self.assertTrue(any("merge_group" in item for item in violations))

    def test_rejects_path_filtered_release_gate(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        stale = """on:
  pull_request:
    branches: [stable]
    paths:
      - docs/**
  merge_group:

jobs:
  release_ok:
    if: always()
"""
        violations = validate_release_gate_workflow(Path("release-gate.yml"), stale, policy)
        self.assertTrue(any("path filters" in item for item in violations))

    def test_rejects_obsolete_ci_ok_aggregate(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        stale = """on:
  push:
    branches: [dev, stable]
  pull_request:
    branches: [dev]

jobs:
  ci_ok:
    runs-on: ubuntu-latest
"""
        violations = validate_integration_workflow(Path("ci.yml"), stale, policy)
        self.assertTrue(any("ci_ok" in item for item in violations))

    def test_live_protection_rejects_ci_ok_and_unbound_app(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        stale_stable = {
            "required_status_checks": {
                "strict": True,
                "contexts": ["ci_ok"],
                "checks": [{"context": "ci_ok", "app_id": None}],
            }
        }
        violations = validate_live_protection(
            stable_protection=stale_stable,
            dev_protection={},
            policy=policy,
        )
        self.assertTrue(any("ci_ok" in item for item in violations))

    def test_live_protection_accepts_github_actions_release_ok(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        stable = {
            "required_status_checks": {
                "strict": True,
                "contexts": ["release_ok"],
                "checks": [{"context": "release_ok", "app_id": GITHUB_ACTIONS_APP_ID}],
            }
        }
        self.assertEqual(
            validate_live_protection(
                stable_protection=stable,
                dev_protection={},
                policy=policy,
            ),
            [],
        )

    def test_live_protection_rejects_required_checks_on_dev(self):
        policy = parse_documented_policy_full(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        stable = {
            "required_status_checks": {
                "checks": [{"context": "release_ok", "app_id": GITHUB_ACTIONS_APP_ID}],
            }
        }
        dev = {
            "required_status_checks": {
                "checks": [{"context": "release_ok", "app_id": GITHUB_ACTIONS_APP_ID}],
            }
        }
        violations = validate_live_protection(
            stable_protection=stable,
            dev_protection=dev,
            policy=policy,
        )
        self.assertTrue(any("dev must not require" in item for item in violations))


if __name__ == "__main__":
    unittest.main()
