#!/usr/bin/env python3
"""Unit tests for the GUI preflight-truth guard.

Every rule gets a planted-violation case (a snippet that must be reported) and
an allowed case (a Core-owned surface that must not be). The snippets are plain
strings rather than edits to real GUI files: the guard is run over the tree, so
planting a violation in the tree would make the guard fail its own repository.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.check_preflight_truth_source import (  # noqa: E402
    ALLOWED_SURFACES,
    RULES,
    gui_sources,
    in_scope,
    scan_text,
    scan_tree,
)


PREFLIGHT_PANE = "ProductQuickAccessibilitySmoke/qml/PreflightPane.qml"
MAIN_QML = "LoopEditor/qml/Main.qml"
EDITOR_HOST = "LoopEditor/editorhost.cpp"
SMOKE_MAIN = "ProductQuickAccessibilitySmoke/main.cpp"

# One representative usage per allowed surface. The key set is asserted against
# ALLOWED_SURFACES so a newly allow-listed name must arrive with a usage here.
CORE_OWNED_USAGE = {
    "preflightOperatorSummary": "text: root.host.preflightOperatorSummary",
    "preflightStateName": 'enabled: root.host.preflightStateName !== "running"',
    "preflightStateVisual": "property var stateVisual: host ? host.preflightStateVisual : ({})",
    "preflightStateColor": 'color: root.host ? root.host.preflightStateColor : "transparent"',
    "preflightProfiles": "model: root.host ? root.host.preflightProfiles : []",
    "preflightVariables": "model: root.host ? root.host.preflightVariables : []",
    "selectedPreflightProfileId": "if (model[i].id === root.host.selectedPreflightProfileId) return i",
    "hasPreflightReport": "enabled: root.host && root.host.hasPreflightReport",
    "runPreflight": "onClicked: if (root.host) root.host.runPreflight()",
    "cancelPreflight": "onClicked: if (root.host) root.host.cancelPreflight()",
    "selectPreflightProfile": "onActivated: if (root.host) root.host.selectPreflightProfile(currentValue)",
    "setPreflightVariable": "onEditingFinished: root.host.setPreflightVariable(model.name, text)",
    "requestPreflightReportExport": "onClicked: if (root.host) root.host.requestPreflightReportExport()",
    "exportPreflightReportFileUrl": "onAccepted: if (host) host.exportPreflightReportFileUrl(selectedFile)",
}


class PreflightTruthSourceTest(unittest.TestCase):
    # -- rule: the GUI derives no completeness ---------------------------------
    def test_flags_inspection_complete_in_qml(self) -> None:
        violations = scan_text(PREFLIGHT_PANE, "visible: preflight.inspectionComplete")
        self.assertEqual([violation.rule for violation in violations], ["gui-derives-completeness"])

    def test_flags_inspection_complete_in_the_editorhost_presentation_layer(self) -> None:
        violations = scan_text(EDITOR_HOST, "const bool pass = !result.inspectionComplete;")
        self.assertEqual([violation.rule for violation in violations], ["gui-derives-completeness"])

    # -- rule: the GUI aggregates no findings ----------------------------------
    def test_flags_a_verdict_derived_from_the_error_count(self) -> None:
        violations = scan_text(PREFLIGHT_PANE, 'text: result.errors.length === 0 ? "PASS" : "FAIL"')
        self.assertIn("gui-aggregates-findings", {violation.rule for violation in violations})

    def test_flags_errors_is_empty_in_qml(self) -> None:
        violations = scan_text(PREFLIGHT_PANE, "if (result.errors.isEmpty()) status = ok")
        self.assertEqual([violation.rule for violation in violations], ["gui-aggregates-findings"])

    def test_flags_warning_count_aggregation(self) -> None:
        violations = scan_text(PREFLIGHT_PANE, "enabled: warnings.length > 0")
        self.assertEqual([violation.rule for violation in violations], ["gui-aggregates-findings"])

    # -- rule: the GUI writes no pass/fail copy --------------------------------
    def test_flags_a_pass_fail_literal_without_any_aggregation(self) -> None:
        violations = scan_text(PREFLIGHT_PANE, 'text: result.ok ? qsTr("PASS") : qsTr("FAIL")')
        self.assertEqual([violation.rule for violation in violations], ["gui-derives-pass-fail-copy"])

    def test_reports_the_path_and_line_of_each_violation(self) -> None:
        violations = scan_text(MAIN_QML, "import QtQuick\n\nvar pass = result.inspectionComplete\n")
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0].line, 3)
        self.assertTrue(violations[0].format().startswith(f"{MAIN_QML}:3: gui-derives-completeness"))

    # -- allowed: the Core-owned surfaces --------------------------------------
    def test_allows_every_core_owned_surface(self) -> None:
        self.assertEqual(sorted(CORE_OWNED_USAGE), sorted(ALLOWED_SURFACES))
        for surface, usage in CORE_OWNED_USAGE.items():
            with self.subTest(surface=surface):
                self.assertEqual(scan_text(PREFLIGHT_PANE, usage), [])

    def test_allows_reading_core_worded_prose_in_editorhost(self) -> None:
        text = "QString EditorHost::preflightOperatorSummary() const\n{\n    return m_preflight.operatorSummary();\n}\n"
        self.assertEqual(scan_text(EDITOR_HOST, text), [])

    # -- scope -----------------------------------------------------------------
    def test_scope_covers_the_gui_layers_only(self) -> None:
        self.assertTrue(in_scope(PREFLIGHT_PANE))
        self.assertTrue(in_scope(MAIN_QML))
        self.assertTrue(in_scope(SMOKE_MAIN))
        self.assertTrue(in_scope(EDITOR_HOST))
        self.assertFalse(in_scope("LoopLibCore/sources/pdfpreflightverdict.cpp"))
        self.assertFalse(in_scope("LoopLibInteraction/sources/preflightcontroller.cpp"))
        self.assertFalse(in_scope("PdfTool/pdftoolpreflight.cpp"))
        self.assertFalse(in_scope("UnitTests/tst_editorhosttest.cpp"))
        self.assertFalse(in_scope("docs/LOOP_SHELL_CONTRACT.md"))

    def test_scope_finds_the_real_gui_sources(self) -> None:
        sources = gui_sources()
        self.assertIn(EDITOR_HOST, sources)
        self.assertIn(SMOKE_MAIN, sources)
        self.assertIn(PREFLIGHT_PANE, sources)
        self.assertIn(MAIN_QML, sources)
        self.assertGreaterEqual(len(sources), 20)

    # -- the guard itself ------------------------------------------------------
    def test_the_real_tree_computes_no_preflight_truth(self) -> None:
        self.assertEqual([violation.format() for violation in scan_tree()], [])

    def test_every_rule_documents_why_it_exists(self) -> None:
        self.assertEqual(len({rule.id for rule in RULES}), len(RULES))
        for rule in RULES:
            with self.subTest(rule=rule.id):
                self.assertTrue(rule.detail.strip())

    def test_every_allowed_surface_documents_why_it_is_allowed(self) -> None:
        for surface, reason in ALLOWED_SURFACES.items():
            with self.subTest(surface=surface):
                self.assertTrue(reason.strip())

    def test_no_allowed_surface_matches_a_rule(self) -> None:
        """The allow-list must never be used to launder a forbidden pattern."""
        for forbidden in ("errors.length", "warnings.length", "errors.isEmpty", "inspectionComplete", '"PASS"'):
            self.assertNotIn(forbidden, ALLOWED_SURFACES)


if __name__ == "__main__":
    unittest.main()
