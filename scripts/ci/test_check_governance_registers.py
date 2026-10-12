#!/usr/bin/env python3
"""Negative cases for source qualification and complete proof ownership."""

from __future__ import annotations

import copy
import unittest

from scripts.ci import check_governance_registers as checker


class GovernanceRegisterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sources, self.proofs = checker.read_registers(checker.ROOT)

    def test_registers_cover_current_contracts(self) -> None:
        self.assertEqual(checker.validate_data(checker.ROOT, self.sources, self.proofs), [])

    def test_missing_module_fails(self) -> None:
        self.sources["modules"].pop()
        self.assertTrue(any("cover L01-L14" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_missing_pending_metric_requires_a_disposition(self) -> None:
        self.proofs["obligations"].pop()
        self.assertTrue(any("every pending" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_declared_metric_cannot_be_promoted_by_the_register(self) -> None:
        self.proofs["obligations"][-1]["enforcement"] = "enforced"
        self.assertTrue(any("enforcement disagrees" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_inventory_cannot_invent_a_measurement_baseline(self) -> None:
        self.proofs["obligations"][-1]["measured_baseline"] = 1
        self.assertTrue(any("does not admit" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_legacy_product_follow_up_cannot_replace_the_reviewed_owner(self) -> None:
        row = next(iter(self.sources["product_follow_ups"].values()))
        row["issue"] = row["legacy"]
        self.assertTrue(any("follow-up identities disagree" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_missing_evidence_is_rejected(self) -> None:
        self.proofs["obligations"][0]["evidence"] = ["unavailable-measurement.json"]
        self.assertTrue(any("does not resolve" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_wrong_repository_issue_identity_is_rejected(self) -> None:
        issue = next(iter(self.sources["issues"].values()))
        issue["repository"] = "legacy"
        self.assertTrue(any("identity disagrees" in error for error in
                            checker.validate_data(checker.ROOT, self.sources, self.proofs)))

    def test_number_collision_fails_live_read_back(self) -> None:
        snapshot = {"47": {"number": 47, "title": "L06-01", "state": "OPEN", "milestone": None}}
        live = {47: {"number": 47, "title": "Legacy bleed calibration", "state": "OPEN", "milestone": None}}
        self.assertTrue(checker.tracker_findings(copy.deepcopy(snapshot), live))

    def test_missing_live_issue_cannot_be_a_provenance_match(self) -> None:
        self.assertTrue(checker.tracker_findings({"47": {}}, {}))


if __name__ == "__main__":
    unittest.main()
