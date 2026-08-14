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
        self.assertIn("GENERATED FILE", rendered)
        self.assertIn("check-change.py --base origin/dev", rendered)
        self.assertIn("Every PR adds exactly one", rendered)


if __name__ == "__main__":
    unittest.main()
