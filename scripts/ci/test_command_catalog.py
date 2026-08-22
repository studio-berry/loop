#!/usr/bin/env python3
"""Exercise the command catalog verifier.

The happy path runs the real repository through the CLI. The negative fixtures
matter more: a parity check that cannot fail is not a parity check.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-command-catalog.py"


def load_verifier():
    spec = importlib.util.spec_from_file_location("verify_command_catalog", VERIFIER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verifier = load_verifier()


MAIN_WINDOW = """
void PDFEditorMainWindow::onActionsInitialized()
{
    m_actionManager->setAction(PDFActionManager::Open, ui->actionOpen);
    m_actionManager->setAction(PDFActionManager::Close, ui->actionClose);
    m_actionManager->setAction(PDFActionManager::BleedFixup, ui->actionBleedFixup);
    m_actionManager->setAction(PDFActionManager::About, ui->actionAbout);
}
"""

CONTROLLER = """
void PDFActionManager::initActions(QSize iconSize, bool initializeStampActions)
{
    setShortcut(Open, QKeySequence::Open);
#ifdef Q_OS_WIN
    setShortcut(Close, QKeyCombination(Qt::CTRL, Qt::Key_W));
#else
    setShortcut(Close, QKeySequence::Close);
#endif
    setShortcut(BleedFixup, QKeySequence("Ctrl+Shift+B"));
}
"""


def declared(action_id: str, shortcut: dict | None = None) -> dict:
    command = {
        "label_key": f"command.{action_id}.label",
        "parameters": [],
        "capability": "unclassified",
        "cancellable": False,
        "availability": "declared",
    }
    if shortcut is not None:
        command["shortcut"] = shortcut
    return command


POLICY = {
    "schema_version": 1,
    "issue": 193,
    "source_ui": "LoupeLibGui/pdfeditormainwindow.ui",
    "expected_action_count": 4,
    "actions": [
        {
            "id": "actionOpen",
            "disposition": "KEEP",
            "target": "Document",
            "command": {
                "label_key": "command.actionOpen.label",
                "shortcut": {"standard_key": "Open"},
                "parameters": [{"name": "path", "type": "string", "required": True}],
                "capability": "document.read",
                "cancellable": True,
                "availability": "implemented",
            },
        },
        {
            "id": "actionClose",
            "disposition": "KEEP",
            "target": "Document",
            "command": {
                "label_key": "command.actionClose.label",
                "shortcut": {
                    "standard_key": "Close",
                    "windows": {"sequence": "Ctrl+W"},
                },
                "parameters": [],
                "capability": "document.read",
                "cancellable": False,
                "availability": "implemented",
            },
        },
        {
            "id": "actionBleedFixup",
            "disposition": "ABSORB",
            "target": "Production",
            "command": declared("actionBleedFixup", {"sequence": "Ctrl+Shift+B"}),
        },
        {
            "id": "actionAbout",
            "disposition": "ADVANCED",
            "target": "Advanced",
            "command": declared("actionAbout"),
        },
    ],
}


class CatalogFixture:
    """A minimal tree the verifier can be pointed at."""

    def __init__(
        self,
        stack: unittest.TestCase,
        policy: dict | None = None,
        main_window: str = MAIN_WINDOW,
        controller: str = CONTROLLER,
    ):
        self.root = Path(stack.enterContext(tempfile.TemporaryDirectory()))
        self.policy_path = self.root / "loupe-shell-actions.json"
        self.main_window_path = self.root / "pdfeditormainwindow.cpp"
        self.controller_path = self.root / "pdfprogramcontroller.cpp"
        self.write(policy if policy is not None else copy.deepcopy(POLICY))
        self.main_window_path.write_text(main_window, encoding="utf-8")
        self.controller_path.write_text(controller, encoding="utf-8")

    def write(self, policy: dict) -> None:
        self.policy_path.write_text(json.dumps(policy), encoding="utf-8")

    def run(self) -> list[str]:
        return verifier.validate_catalog(
            self.policy_path, self.main_window_path, self.controller_path
        )


class CommandCatalogTests(unittest.TestCase):
    def setUp(self):
        # The fixture's implemented set must match the verifier's, or every
        # negative case would drown in unrelated implemented-set errors.
        self._original = verifier.IMPLEMENTED_COMMANDS
        verifier.IMPLEMENTED_COMMANDS = frozenset({"actionOpen", "actionClose"})
        self.addCleanup(setattr, verifier, "IMPLEMENTED_COMMANDS", self._original)

    def mutate(self, action_id: str, **changes) -> list[str]:
        policy = copy.deepcopy(POLICY)
        for action in policy["actions"]:
            if action["id"] == action_id:
                action["command"].update(changes)
        return CatalogFixture(self, policy=policy).run()

    def test_repository_catalog_holds(self):
        result = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertIn("Command catalog verified", result.stdout)

    def test_clean_fixture_passes(self):
        self.assertEqual(CatalogFixture(self).run(), [])

    def test_missing_command_block_is_rejected(self):
        policy = copy.deepcopy(POLICY)
        del policy["actions"][3]["command"]
        errors = CatalogFixture(self, policy=policy).run()
        self.assertTrue(any("no command descriptor" in error for error in errors), errors)

    def test_wrong_label_key_is_rejected(self):
        errors = self.mutate("actionAbout", label_key="about.label")
        self.assertTrue(any("label_key" in error for error in errors), errors)

    def test_unknown_capability_is_rejected(self):
        errors = self.mutate("actionAbout", capability="document.everything")
        self.assertTrue(any("capability" in error for error in errors), errors)

    def test_implemented_command_may_not_stay_unclassified(self):
        errors = self.mutate("actionOpen", capability="unclassified")
        self.assertTrue(any("real capability" in error for error in errors), errors)

    def test_declared_command_may_not_promise_parameters(self):
        errors = self.mutate(
            "actionAbout",
            parameters=[{"name": "path", "type": "string", "required": True}],
        )
        self.assertTrue(any("never reads" in error for error in errors), errors)

    def test_unsupported_parameter_type_is_rejected(self):
        errors = self.mutate(
            "actionOpen",
            parameters=[{"name": "path", "type": "QUrl", "required": True}],
        )
        self.assertTrue(any("unsupported type" in error for error in errors), errors)

    def test_unexpected_implemented_command_is_rejected(self):
        errors = self.mutate("actionAbout", availability="implemented", capability="none")
        self.assertTrue(any("without a handler" in error for error in errors), errors)

    def test_downgrading_an_implemented_command_is_rejected(self):
        errors = self.mutate("actionOpen", availability="declared", parameters=[])
        self.assertTrue(any("no 'implemented' availability" in error for error in errors), errors)

    def test_missing_shortcut_is_rejected(self):
        policy = copy.deepcopy(POLICY)
        del policy["actions"][2]["command"]["shortcut"]
        errors = CatalogFixture(self, policy=policy).run()
        self.assertTrue(any("declares no shortcut" in error for error in errors), errors)

    def test_contradicting_shortcut_is_rejected(self):
        errors = self.mutate("actionBleedFixup", shortcut={"sequence": "Ctrl+B"})
        self.assertTrue(any("contradicts" in error for error in errors), errors)

    def test_invented_shortcut_is_rejected(self):
        errors = self.mutate("actionAbout", shortcut={"sequence": "Ctrl+Shift+A"})
        self.assertTrue(any("invents a shortcut" in error for error in errors), errors)

    def test_windows_override_must_match(self):
        errors = self.mutate(
            "actionClose",
            shortcut={"standard_key": "Close", "windows": {"sequence": "Ctrl+Q"}},
        )
        self.assertTrue(any("contradicts" in error for error in errors), errors)

    def test_action_count_mismatch_is_reported(self):
        policy = copy.deepcopy(POLICY)
        policy["expected_action_count"] = 9
        fixture = CatalogFixture(self, policy=policy)
        with self.assertRaises(verifier.ContractError):
            fixture.run()

    def test_malformed_policy_is_reported(self):
        fixture = CatalogFixture(self)
        fixture.policy_path.write_text(json.dumps({"schema_version": 2}), encoding="utf-8")
        with self.assertRaises(verifier.ContractError):
            fixture.run()

    def test_unreadable_shortcut_branch_is_reported(self):
        controller = CONTROLLER.replace("#else", "#elif defined(Q_OS_MAC)")
        fixture = CatalogFixture(self, controller=controller)
        with self.assertRaises(verifier.ContractError):
            fixture.run()

    def test_missing_action_mapping_is_reported(self):
        fixture = CatalogFixture(self, main_window="// no setAction calls here\n")
        with self.assertRaises(verifier.ContractError):
            fixture.run()


if __name__ == "__main__":
    unittest.main()
