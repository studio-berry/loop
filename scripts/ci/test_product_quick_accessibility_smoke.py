#!/usr/bin/env python3
"""Contract tests for the shipped Quick accessibility smoke surface."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "LoopEditor" / "qml"
MIRROR = ROOT / "ProductQuickAccessibilitySmoke" / "qml"
SMOKE_MAIN = ROOT / "ProductQuickAccessibilitySmoke" / "main.cpp"


class ProductQuickAccessibilitySmokeContractTests(unittest.TestCase):
    def test_product_qml_mirror_is_byte_identical(self) -> None:
        source_files = sorted(SOURCE.glob("*.qml"))
        self.assertTrue(source_files)
        self.assertEqual(
            [path.name for path in source_files],
            [path.name for path in sorted(MIRROR.glob("*.qml"))],
        )
        for source in source_files:
            self.assertEqual(source.read_bytes(), (MIRROR / source.name).read_bytes(), source.name)

    def test_keyboard_and_operator_surfaces_have_stable_names(self) -> None:
        expected = {
            "Main.qml": ("loopMainWindow",),
            "Workspace.qml": ("workspace", "workspaceRail", "workspaceStack"),
            "ShellToolBar.qml": (
                "shellToolBar",
                "openDocumentButton",
                "saveAsDocumentButton",
            ),
            "DocumentPane.qml": ("documentPane", "pagesView", "outlineView", "searchField"),
            "PreflightPane.qml": (
                "preflightPane",
                "preflightProfileSelector",
                "runPreflightButton",
                "cancelPreflightButton",
                "exportPreflightReportButton",
                "preflightFindingsView",
            ),
            "InspectorPane.qml": ("inspectorPane", "inspectorView"),
            "CanvasPane.qml": ("canvasPane", "documentCanvas"),
        }
        for filename, object_names in expected.items():
            text = (SOURCE / filename).read_text(encoding="utf-8")
            for object_name in object_names:
                self.assertIn(f'objectName: "{object_name}"', text, f"{filename}: {object_name}")

    def test_qml_only_emits_intents_and_smoke_checks_truth_and_backend(self) -> None:
        for path in sorted(SOURCE.glob("*.qml")):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("inspectionComplete", text, path.name)
            self.assertNotIn("errors.length", text, path.name)
            self.assertNotIn("warnings.length", text, path.name)
        smoke = SMOKE_MAIN.read_text(encoding="utf-8")
        for marker in (
            "verifyNamedAccessibility",
            "verifyKeyboardSurface",
            "preflightStateVisual",
            "QT_QUICK_BACKEND",
            "focusRestoration()->remember",
            "focusRestoration()->restore",
        ):
            self.assertIn(marker, smoke)


if __name__ == "__main__":
    unittest.main()
