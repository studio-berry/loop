#!/usr/bin/env python3
"""Focused tests for canonical agent policy validation and rendering."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("generate-adapters.py")
SPEC = importlib.util.spec_from_file_location("generate_adapters", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class GenerateAdapterTests(unittest.TestCase):
    def test_repository_policy_is_valid(self) -> None:
        policy = MODULE.load_policy()
        self.assertEqual(policy["changelog"]["required_per_pr"], 1)
        self.assertIn("AGENTS.md", policy["adapters"])

    def test_render_has_generated_marker_and_verification_contract(self) -> None:
        policy = MODULE.load_policy()
        rendered = MODULE.render(policy, "AGENTS.md")
        version, prerelease = MODULE.load_version_policy()
        display_version = MODULE.format_product_version(version, prerelease)
        self.assertIn("GENERATED FILE", rendered)
        self.assertIn(f"version: `{display_version}`", rendered)
        self.assertIn("check-change.py --base origin/dev", rendered)
        self.assertIn("Every PR adds exactly one", rendered)


    def test_render_carries_every_contract_rule_and_anti_slop_item(self) -> None:
        policy = MODULE.load_policy()
        rendered = MODULE.render(policy, "AGENTS.md")
        self.assertIn("## Engineering contract", rendered)
        self.assertIn("## Change review", rendered)
        # The bullets below are emitted from the policy, so an emptied list would
        # make the loops vacuous instead of failing.
        self.assertGreaterEqual(len(policy["engineering_contract"]["rules"]), 5)
        self.assertGreaterEqual(len(policy["change_review"]["anti_slop"]), 5)
        for rule in policy["engineering_contract"]["rules"]:
            self.assertIn(f"- {rule}", rendered)
        for item in policy["change_review"]["anti_slop"]:
            self.assertIn(f"- {item}", rendered)
        self.assertIn(policy["change_review"]["preserve"], rendered)
        for line in policy["change_review"]["closing"]:
            self.assertIn(line, rendered)

    def test_change_review_keeps_safety_handling_and_shuns_style_metrics(self) -> None:
        policy = MODULE.load_policy()
        review = policy["change_review"]
        for preserved in ("validation", "security", "cancellation", "provenance", "failure handling"):
            self.assertIn(preserved, review["preserve"])
        self.assertTrue(any("automated style metrics" in line for line in review["closing"]))
        self.assertTrue(any("verification" in line for line in review["closing"]))

    def test_engineering_contract_is_scoped_to_touched_code(self) -> None:
        policy = MODULE.load_policy()
        self.assertIn("behavior-bearing", policy["engineering_contract"]["applies_to"])
        self.assertIn("never refactor the repository", policy["engineering_contract"]["applies_to"])


if __name__ == "__main__":
    unittest.main()
