#!/usr/bin/env python3
"""Unit tests for the preflight check-catalog and coverage-backlog generator.

Mirrors scripts/ci/test_correction_operation_catalog.py: the enriched check rows
and the prioritised backlog are generated artifacts, so each required field and
each cross-reference is asserted to fail closed when the overlay drops it.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

GENERATOR_PATH = ROOT / "scripts" / "generate-architecture-catalogs.py"
spec = importlib.util.spec_from_file_location("generate_architecture_catalogs", GENERATOR_PATH)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
OVERLAY_PATH = ROOT / "docs" / "preflight-check-catalog-overlay.json"


class PreflightCheckCatalogTest(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = generator.parse_preflight_checks()
        self.operations = generator.parse_repair_operations()

    def overlay(self) -> dict:
        return json.loads(OVERLAY_PATH.read_text(encoding="utf-8"))

    def assert_overlay_fails(self, callback, pattern: str, target: str = "catalog") -> None:
        """A mutated overlay must fail closed in the named validator."""
        overlay = self.overlay()
        callback(overlay)
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "overlay.json"
            path.write_text(json.dumps(overlay), encoding="utf-8")
            original = generator.PREFLIGHT_OVERLAY_PATH
            generator.PREFLIGHT_OVERLAY_PATH = path
            try:
                with self.assertRaisesRegex(ValueError, pattern):
                    if target == "catalog":
                        generator.build_preflight_check_catalog(self.registry, self.operations)
                    else:
                        generator.build_preflight_backlog(overlay, self.registry, self.operations)
            finally:
                generator.PREFLIGHT_OVERLAY_PATH = original

    def test_registry_and_overlay_are_in_sync(self) -> None:
        catalog = generator.build_preflight_check_catalog(self.registry, self.operations)
        self.assertEqual(catalog["registry"], sorted(self.registry))
        self.assertEqual(catalog["fixup_registry"], generator.preflight_fixup_ids(self.operations))

    def test_every_check_row_carries_the_enriched_fields(self) -> None:
        catalog = generator.build_preflight_check_catalog(self.registry, self.operations)
        for check_id, entry in catalog["checks"].items():
            for field in ("parameters", "severity", "evidence", "fixups"):
                self.assertIn(field, entry, f"{check_id} is missing {field}")
            self.assertTrue(entry["severity"], f"{check_id} has no severity model")
            self.assertTrue(entry["evidence"], f"{check_id} has no evidence fields")

    def test_generated_catalog_matches_committed_copy(self) -> None:
        expected = generator.serialized_preflight_catalog()
        actual = (ROOT / "docs" / "generated" / "preflight-check-catalog.json").read_text(encoding="utf-8")
        self.assertEqual(actual, expected)

    def test_generated_backlog_matches_committed_copy(self) -> None:
        expected = generator.serialized_preflight_backlog()
        actual = (ROOT / "docs" / "generated" / "preflight-coverage-backlog.json").read_text(encoding="utf-8")
        self.assertEqual(actual, expected)

    def test_missing_overlay_entry_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["checks"].pop("color-mode"),
            "registered without catalog: color-mode",
        )

    def test_missing_enriched_field_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["checks"]["bleed"].pop("fixups"),
            "catalog entry 'bleed' missing fixups",
        )

    def test_unknown_parameter_id_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["checks"]["bleed"]["parameters"].append(
                {"id": "not_a_profile_field", "type": "number", "default": 1, "range": "> 0", "meaning": "x"}
            ),
            "is not a profile check field the engine reads",
        )

    def test_unregistered_fixup_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["checks"]["bleed"].__setitem__("fixups", ["standards-convert"]),
            "is not a registered preflight fixup",
        )

    def test_unclaimed_registered_fixup_fails(self) -> None:
        def drop(overlay: dict) -> None:
            for entry in overlay["checks"].values():
                entry["fixups"] = []

        self.assert_overlay_fails(drop, "registered preflight fixups that no catalog entry claims")

    def test_invalid_severity_value_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["checks"]["bleed"]["severity"][0].__setitem__("severity", "fatal"),
            "invalid severity",
        )

    def test_invalid_evidence_field_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["checks"]["bleed"]["evidence"].append("finding.nope"),
            "is neither a report finding field",
        )

    def test_duplicate_backlog_id_fails(self) -> None:
        def duplicate(overlay: dict) -> None:
            overlay["backlog"].append(copy.deepcopy(overlay["backlog"][0]))

        self.assert_overlay_fails(duplicate, "duplicate id", target="backlog")

    def test_invalid_priority_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["backlog"][0].__setitem__("priority", "P4"),
            "invalid priority",
            target="backlog",
        )

    def test_unknown_family_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["backlog"][0].__setitem__("families", ["screen-print"]),
            "names unknown families: screen-print",
            target="backlog",
        )

    def test_unverified_issue_reference_fails(self) -> None:
        def invent(overlay: dict) -> None:
            for row in overlay["backlog"]:
                if row["closed_by"] == "unfiled":
                    row["closed_by"] = "#999999"
                    break

        self.assert_overlay_fails(invent, "references unverified issue #999999", target="backlog")

    def test_state_disagreeing_with_issue_state_fails(self) -> None:
        def mismatch(overlay: dict) -> None:
            for row in overlay["backlog"]:
                if row["closed_by"] == "#605":
                    row["state"] = "closed"
                    break

        self.assert_overlay_fails(mismatch, "disagrees with #605", target="backlog")

    def test_uncovered_class_missing_from_backlog_fails(self) -> None:
        def drop(overlay: dict) -> None:
            overlay["backlog"] = [row for row in overlay["backlog"] if row["priority"] != "P1"]

        self.assert_overlay_fails(drop, "uncovered class is missing from the backlog", target="backlog")

    def test_extra_backlog_key_fails(self) -> None:
        self.assert_overlay_fails(
            lambda overlay: overlay["backlog"][0].__setitem__("owner", "nobody"),
            "must carry exactly",
            target="backlog",
        )


if __name__ == "__main__":
    unittest.main()
