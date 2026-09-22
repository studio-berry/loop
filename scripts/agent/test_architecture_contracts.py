#!/usr/bin/env python3
"""Tests for architecture contracts, evidence manifests, and quality budgets."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name("check-architecture.py")
SPEC = importlib.util.spec_from_file_location("check_architecture", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

YAML_SPEC = importlib.util.spec_from_file_location(
    "yaml_subset", Path(__file__).with_name("yaml_subset.py")
)
assert YAML_SPEC and YAML_SPEC.loader
YAML = importlib.util.module_from_spec(YAML_SPEC)
YAML_SPEC.loader.exec_module(YAML)


class ArchitectureContractTests(unittest.TestCase):
    def test_repo_contracts_pass(self) -> None:
        errors = MODULE.static_errors(ROOT)
        self.assertEqual(errors, [], "\n".join(errors))

    def test_yaml_subset_matches_pyyaml(self) -> None:
        import yaml

        paths = list((ROOT / "architecture").glob("*.yaml"))
        paths += list((ROOT / "UnitTests/testdata/fixture-classes").rglob("*.yaml"))
        for path in paths:
            text = path.read_text(encoding="utf-8")
            self.assertEqual(YAML.load_yaml(text), yaml.safe_load(text), path)

    def test_forbidden_include_is_detected(self) -> None:
        patterns = [__import__("re").compile(r"^QWidget$")]
        hits = MODULE.include_hits('#include "QWidget"\n', patterns)
        self.assertEqual(hits, ["QWidget"])

    def test_sealed_fixture_requires_approval_marker(self) -> None:
        fixtures_doc = YAML.load_yaml((ROOT / "architecture/fixtures.yaml").read_text(encoding="utf-8"))
        error = MODULE.sealed_change_error(None, fixtures_doc)
        self.assertIsNotNone(error)
        self.assertIn("sealed_output_approval", error or "")
        self.assertIsNone(MODULE.sealed_change_error({"sealed_output_approval": "human"}, fixtures_doc))

    def test_development_fixture_cannot_satisfy_sealed_claim(self) -> None:
        budgets = YAML.load_yaml((ROOT / "architecture/quality-budgets.yaml").read_text(encoding="utf-8"))
        fixtures_doc = YAML.load_yaml((ROOT / "architecture/fixtures.yaml").read_text(encoding="utf-8"))
        _fixture_errors, fixtures = MODULE.check_fixtures(ROOT, fixtures_doc, budgets)
        manifest = {
            "claims": [
                {
                    "id": "bad-seal",
                    "lane": "sealed-eval",
                    "evidence": ["fixture:development:search-iteration"],
                }
            ],
            "unresolved": [],
        }
        errors = MODULE.check_evidence(
            ROOT,
            manifest,
            "changes/example.evidence.yaml",
            [],
            {},
            fixtures,
            budgets,
            True,
        )
        self.assertTrue(any("development fixture" in error for error in errors), errors)

    def test_unknown_budget_ref_fails(self) -> None:
        errors = MODULE.check_evidence(
            ROOT,
            {"claims": [{"id": "budgets", "evidence": ["budget:missing"]}], "unresolved": []},
            "changes/example.evidence.yaml",
            [],
            {},
            {},
            {"metrics": []},
            True,
        )
        self.assertTrue(any("unknown budget" in error for error in errors), errors)

    def test_loosened_budget_requires_unresolved_marker(self) -> None:
        previous = {"metrics": [{"id": "rendering-channel-delta", "maximum": 2, "agent_tunable": False}]}
        current = {
            "metrics": [
                {
                    "id": "rendering-channel-delta",
                    "maximum": 4,
                    "agent_tunable": False,
                    "fixture_class": "regression",
                }
            ]
        }
        errors = MODULE.compare_budget_documents(previous, current, {"unresolved": []})
        self.assertTrue(any("budget-loosened:rendering-channel-delta" in error for error in errors), errors)

    def test_canvas_budget_over_cap_fails(self) -> None:
        metric = {
            "id": "rendering-channel-delta",
            "baseline_kind": "canvas-parity-field",
            "baseline": "UnitTests/testdata/canvas-parity/budgets.json",
            "baseline_field": "max_channel_delta_budget",
            "maximum": 0,
        }
        errors = MODULE.measure_budget(ROOT, metric)
        self.assertTrue(errors)


if __name__ == "__main__":
    unittest.main()
