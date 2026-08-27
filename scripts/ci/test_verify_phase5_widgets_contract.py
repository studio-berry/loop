"""Negative coverage for the Phase 5 Widgets evidence join."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from generate_phase5_widgets_evidence import (  # noqa: E402
    _check_or_write,
    _normalize_newlines,
    _proven_owner_ids,
    _serialized,
    generate,
)
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

    def test_check_treats_crlf_checkout_as_current(self):
        self.assertEqual(_normalize_newlines("a\r\nb\n"), "a\nb\n")
        crlf = _serialized(self.inventory).replace("\n", "\r\n")
        self.assertEqual(_normalize_newlines(crlf), _serialized(self.inventory))
        self.assertEqual(_check_or_write(ROOT, write=False), [])

    def test_invalid_disposition_fails_closed(self):
        disposition = copy.deepcopy(self.disposition)
        disposition["rows"][0]["disposition"] = "DEFAULT"
        errors = validate_disposition(self.inventory, disposition, ROOT)
        self.assertTrue(any("invalid disposition" in error for error in errors))

    def test_secondary_executables_are_frozen_to_installed_owners(self):
        rows = {row["target"]: row for row in self.disposition["rows"] if "target" in row}
        expected = {
            "LoupeViewer": ("DELETE", "loupe-editor", "STOP-SHIPPING"),
            "LoupeLaunchPad": ("DELETE", "loupe-editor", "STOP-SHIPPING"),
            "LoupePageMaster": ("HEADLESS-REPLACE", "loupe-cli", "CLI-ONLY"),
            "LoupeDiff": ("HEADLESS-REPLACE", "loupe-cli", "CLI-ONLY"),
        }
        for target, (disposition, replacement, source) in expected.items():
            row = rows[target]
            self.assertEqual(row["disposition"], disposition, target)
            self.assertEqual(row["replacement_target"], replacement, target)
            self.assertEqual(row["source_disposition"], source, target)
            self.assertIn(replacement, row["testable_condition"], target)
            self.assertNotRegex(row["rationale"], r"Issue \d+", target)
            self.assertNotRegex(row["testable_condition"], r"Issue \d+", target)
            self.assertNotEqual(row["disposition"], "BLOCKED", target)
        blocked = [row["id"] for row in self.disposition["rows"] if row["disposition"] == "BLOCKED"]
        self.assertEqual(blocked, ["target:RedactPlugin"])
        ocr = rows["OcrPlugin"]
        self.assertEqual(ocr["replacement_target"], "loupe-cli")
        self.assertIn("only after loupe-cli is proven", ocr["testable_condition"])
        self.assertNotIn("may leave the install graph", ocr["testable_condition"])
        product = json.loads((ROOT / "docs" / "product-surface.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(_proven_owner_ids(product)), ["loupe-cli", "loupe-editor"])
        by_id = {row["id"]: row for row in product["surfaces"]}
        for surface_id in ("loupe-viewer", "loupe-pagemaster", "loupe-diff", "loupe-launchpad"):
            row = by_id[surface_id]
            self.assertEqual(row["artifact_scope"], "build", surface_id)
            self.assertEqual(row["profiles"]["developer"], "absent", surface_id)
            self.assertEqual(row["profiles"]["loupe-release"], "absent", surface_id)
        packaging = product["packaging"]
        self.assertEqual(packaging["appx_applications"]["developer"], ["LoupeEditor"])
        self.assertEqual(packaging["appx_applications"]["loupe-release"], ["LoupeEditor"])
        for name in ("LoupeViewer", "LoupePageMaster", "LoupeDiff", "LoupeLaunchPad"):
            target = next(row for row in self.inventory["targets"] if row["id"] == name)
            self.assertFalse(target["install_rule"], name)
            self.assertFalse(target["installed_in_profile"], name)

    def test_loupe_editor_is_sole_installed_interactive_product(self):
        completed = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "verify-installed-product-graph.py")],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)


if __name__ == "__main__":
    unittest.main()
