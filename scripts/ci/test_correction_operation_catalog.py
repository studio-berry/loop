#!/usr/bin/env python3
"""Unit tests for the correction-operation catalog generator.

Mirrors the preflight check-catalog gate: every registered repair operation
must have exactly one overlay row, and overlay revalidation class must match
the impact/save policy parsed from Core source.
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
OVERLAY_PATH = ROOT / "docs" / "correction-operation-catalog-overlay.json"


class CorrectionOperationCatalogTest(unittest.TestCase):
    def test_registry_and_overlay_are_in_sync(self) -> None:
        registry = generator.parse_repair_operations()
        catalog = generator.build_correction_operation_catalog(registry)
        self.assertEqual(catalog["registry"], sorted(operation["id"] for operation in registry))

    def test_generated_catalog_matches_committed_copy(self) -> None:
        expected = generator.serialized_correction_catalog()
        actual = (ROOT / "docs" / "generated" / "correction-operation-catalog.json").read_text(encoding="utf-8")
        self.assertEqual(actual, expected)

    def test_missing_overlay_entry_fails(self) -> None:
        registry = generator.parse_repair_operations()
        with tempfile.TemporaryDirectory() as temp_dir:
            overlay = json.loads(OVERLAY_PATH.read_text(encoding="utf-8"))
            overlay["operations"].pop("add-bleed")
            path = Path(temp_dir) / "overlay.json"
            path.write_text(json.dumps(overlay), encoding="utf-8")
            original = generator.CORRECTION_OVERLAY_PATH
            generator.CORRECTION_OVERLAY_PATH = path
            try:
                with self.assertRaisesRegex(ValueError, "registered without catalog: add-bleed"):
                    generator.build_correction_operation_catalog(registry)
            finally:
                generator.CORRECTION_OVERLAY_PATH = original

    def test_extra_overlay_entry_fails(self) -> None:
        registry = generator.parse_repair_operations()
        with tempfile.TemporaryDirectory() as temp_dir:
            overlay = json.loads(OVERLAY_PATH.read_text(encoding="utf-8"))
            overlay["operations"]["not-registered"] = copy.deepcopy(overlay["operations"]["add-bleed"])
            path = Path(temp_dir) / "overlay.json"
            path.write_text(json.dumps(overlay), encoding="utf-8")
            original = generator.CORRECTION_OVERLAY_PATH
            generator.CORRECTION_OVERLAY_PATH = path
            try:
                with self.assertRaisesRegex(ValueError, "catalog without registry: not-registered"):
                    generator.build_correction_operation_catalog(registry)
            finally:
                generator.CORRECTION_OVERLAY_PATH = original

    def test_revalidation_class_mismatch_fails(self) -> None:
        registry = generator.parse_repair_operations()
        with tempfile.TemporaryDirectory() as temp_dir:
            overlay = json.loads(OVERLAY_PATH.read_text(encoding="utf-8"))
            overlay["operations"]["add-bleed"]["revalidation"]["class"] = "full"
            path = Path(temp_dir) / "overlay.json"
            path.write_text(json.dumps(overlay), encoding="utf-8")
            original = generator.CORRECTION_OVERLAY_PATH
            generator.CORRECTION_OVERLAY_PATH = path
            try:
                with self.assertRaisesRegex(ValueError, "revalidation.class 'full'"):
                    generator.build_correction_operation_catalog(registry)
            finally:
                generator.CORRECTION_OVERLAY_PATH = original

    def test_save_policy_modes_match_repairoperationtest_expectations(self) -> None:
        registry = {operation["id"]: operation for operation in generator.parse_repair_operations()}
        expectations = {
            "add-bleed": "save-as-new-artifact",
            "downsample-images": "full-rewrite",
            "rgb-to-cmyk": "save-as-new-artifact",
            "standards-convert": "full-rewrite",
            "production.validate-wide-format": "incremental-append",
            "production.add-contour-bleed": "save-as-new-artifact",
            "production.place-grommets": "incremental-append",
        }
        for operation_id, mode in expectations.items():
            with self.subTest(operation_id=operation_id):
                self.assertEqual(registry[operation_id]["save_policy"]["mode"], mode)


if __name__ == "__main__":
    unittest.main()
