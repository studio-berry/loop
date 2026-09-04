"""Negative coverage for the Phase 5 Widgets evidence join."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
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
        # No assertion on counts["targets"]: it is the size of an inventory that grows
        # with ordinary development, so pinning it only reports target churn as a
        # contract breach. validate_contract() above checks what the evidence means.
        self.assertEqual(self.inventory["counts"]["widgets_surfaces"], 4)
        self.assertEqual(self.inventory["counts"]["ui_forms"], 2)
        self.assertEqual(len(self.inventory["plugin_ui"]), 0)
        self.assertEqual(len(self.disposition["rows"]), 6)

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

    def test_secondary_executables_are_absent_after_issue_17(self):
        product = json.loads((ROOT / "docs" / "product-surface.json").read_text(encoding="utf-8"))
        by_id = {row["id"]: row for row in product["surfaces"]}
        for surface_id in ("loop-viewer", "loop-pagemaster", "loop-diff", "loop-launchpad"):
            row = by_id[surface_id]
            self.assertEqual(row["artifact_scope"], "build", surface_id)
            self.assertEqual(row["profiles"]["developer"], "absent", surface_id)
            self.assertEqual(row["profiles"]["loop-release"], "absent", surface_id)
        inventory_ids = {row["id"] for row in self.inventory["targets"]}
        for name in ("LoopViewer", "LoopPageMaster", "LoopDiff", "LoopLaunchPad"):
            self.assertNotIn(name, inventory_ids, name)
        for name in ("LoopViewer", "LoopPageMaster", "LoopDiff", "LoopLaunchPad"):
            target = next((row for row in self.inventory["targets"] if row["id"] == name), None)
            self.assertIsNone(target, name)

    def test_profile_options_are_derived_from_product_manifest(self):
        product = json.loads((ROOT / "docs/product-surface.json").read_text(encoding="utf-8"))
        expected = {"LOOP_LOOP_DISTRIBUTION": True}
        for row in product["surfaces"]:
            option = row.get("build_option")
            if option:
                expected[option] = row["profiles"]["loop-release"] == "present"
        self.assertEqual(self.inventory["profile"]["options"], dict(sorted(expected.items())))

    def test_configured_cmake_cache_is_checked_against_manifest_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            (build_dir / "CMakeCache.txt").write_text(
                "\n".join(
                    [
                        "LOOP_LOOP_DISTRIBUTION:BOOL=ON",
                        "LOOP_BUILD_QUICK_CANVAS:BOOL=ON",
                        "LOOP_BUILD_CODE_GENERATOR:BOOL=ON",
                        "LOOP_BUILD_JBIG2_VIEWER:BOOL=OFF",
                        "LOOP_BUILD_EXAMPLE_GENERATOR:BOOL=OFF",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            errors = validate_contract(ROOT, self.inventory, self.disposition, build_dir=build_dir)
        self.assertTrue(any("CMake cache LOOP_BUILD_CODE_GENERATOR" in error for error in errors))

    def test_widgets_plugins_are_absent_after_issue_17(self):
        product = json.loads((ROOT / "docs/product-surface.json").read_text(encoding="utf-8"))
        plugin_rows = [
            row
            for row in product["surfaces"]
            if row.get("kind") == "plugin" and row.get("artifact")
        ]
        for row in plugin_rows:
            self.assertEqual(row["artifact_scope"], "build", row["artifact"])
            self.assertEqual(row["profiles"]["loop-release"], "absent", row["artifact"])
        inventory_ids = {row["id"] for row in self.inventory["targets"]}
        retired_plugins = (
            "ActionListPlugin",
            "DimensionsPlugin",
            "EditorPlugin",
            "LoopPreflightPlugin",
            "ObjectInspectorPlugin",
            "OutputPreviewPlugin",
            "RedactPlugin",
            "ScannerPlugin",
            "SignaturePlugin",
            "SoftProofingPlugin",
            "AudioBookPlugin",
            "OcrPlugin",
        )
        # Assert the set, not its size: a thirteenth plugin then names itself instead
        # of arriving as "13 != 12".
        self.assertEqual({row["artifact"] for row in plugin_rows}, set(retired_plugins))
        for plugin in retired_plugins:
            self.assertNotIn(plugin, inventory_ids, plugin)

    def test_loop_editor_is_sole_installed_interactive_product(self):
        completed = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "verify-installed-product-graph.py")],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)

    def test_plugin_surface_policies_are_executed(self):
        completed = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "verify-plugin-surface-policies.py")],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)

    def test_developer_tool_ui_forms_match_ledger(self):
        shell = json.loads((ROOT / "docs/loop-shell.json").read_text(encoding="utf-8"))
        legacy = shell["legacy_surface_disposition"]
        repo_ui = sorted(
            str(path.relative_to(ROOT)).replace("\\", "/")
            for path in ROOT.rglob("*.ui")
            if path.is_file()
        )
        ledger_paths = sorted(entry["path"] for entry in legacy)
        self.assertEqual(repo_ui, ledger_paths)
        # Tied to the ledger the test just compared against, not to a literal that has
        # to be hand-updated alongside it.
        self.assertEqual(self.inventory["counts"]["ui_forms"], len(ledger_paths))
        self.assertEqual(len(self.disposition["rows"]), 6)


if __name__ == "__main__":
    unittest.main()
