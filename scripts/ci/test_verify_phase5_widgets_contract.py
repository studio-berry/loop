"""Negative coverage for the Phase 5 Widgets evidence join."""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from generate_phase5_widgets_evidence import generate  # noqa: E402
from verify_phase5_widgets_contract import validate_contract, validate_disposition  # noqa: E402


class Phase5WidgetsContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.inventory, cls.disposition = generate(ROOT)

    def test_current_evidence_is_valid_and_complete(self):
        self.assertEqual(validate_contract(ROOT, self.inventory, self.disposition), [])
        self.assertEqual(self.inventory["counts"]["targets"], 89)
        self.assertEqual(self.inventory["counts"]["widgets_surfaces"], 22)
        self.assertEqual(self.inventory["counts"]["ui_forms"], 48)
        self.assertEqual(len(self.inventory["plugin_ui"]), 12)
        self.assertEqual(len(self.disposition["rows"]), 70)

    def test_unknown_surface_fails_closed(self):
        disposition = copy.deepcopy(self.disposition)
        disposition["rows"].append(
            {
                "id": "target:UnknownWidgetsSurface",
                "kind": "application",
                "target": "UnknownWidgetsSurface",
                "disposition": "DELETE",
                "consumer": "none",
                "rationale": "fixture",
                "testable_condition": "fixture",
            }
        )
        errors = validate_disposition(self.inventory, disposition, ROOT)
        self.assertTrue(any("identity mismatch" in error for error in errors))

    def test_missing_disposition_fails_closed(self):
        disposition = copy.deepcopy(self.disposition)
        disposition["rows"].pop()
        errors = validate_disposition(self.inventory, disposition, ROOT)
        self.assertTrue(any("identity mismatch" in error for error in errors))

    def test_duplicate_disposition_fails_closed(self):
        disposition = copy.deepcopy(self.disposition)
        disposition["rows"].append(copy.deepcopy(disposition["rows"][0]))
        errors = validate_disposition(self.inventory, disposition, ROOT)
        self.assertTrue(any("duplicate disposition identity" in error for error in errors))

    def test_invalid_disposition_fails_closed(self):
        disposition = copy.deepcopy(self.disposition)
        disposition["rows"][0]["disposition"] = "DEFAULT"
        errors = validate_disposition(self.inventory, disposition, ROOT)
        self.assertTrue(any("invalid disposition" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
