#!/usr/bin/env python3
import unittest
from pathlib import Path

from scripts.ci.check_branch_policy import (
    parse_documented_policy,
    parse_workflow_branch_triggers,
    validate_workflow_branches,
)


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_BRANCHES = ("dev", "stable")


class BranchPolicyTests(unittest.TestCase):
    def test_documented_policy_declares_ci_branches_and_required_check(self):
        branches, required_check = parse_documented_policy(
            (ROOT / "docs" / "BRANCH_POLICY.md").read_text(encoding="utf-8")
        )
        self.assertEqual(branches, EXPECTED_BRANCHES)
        self.assertEqual(required_check, "ci_ok")

    def test_current_ci_workflow_matches_policy(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertEqual(parse_workflow_branch_triggers(workflow)["push"], EXPECTED_BRANCHES)
        self.assertEqual(validate_workflow_branches(Path("ci.yml"), workflow, EXPECTED_BRANCHES), [])

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


if __name__ == "__main__":
    unittest.main()
