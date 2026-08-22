#!/usr/bin/env python3
"""Exercise the repository-level Quick shell admission verifier."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-quick-shell-policy.py"


def load_verifier():
    spec = importlib.util.spec_from_file_location("quick_shell_policy", VERIFIER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {VERIFIER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class QuickShellContractTests(unittest.TestCase):
    def test_repository_policy_is_valid(self):
        result = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertIn("Quick shell policy verified", result.stdout)

    def test_qml_files_ignores_excluded_directories_relative_to_root(self):
        verifier = load_verifier()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / ".worktrees" / "checkout"
            qml = root / "LoupeLibQuick" / "Main.qml"
            qml.parent.mkdir(parents=True)
            qml.write_text("import QtQuick\nItem {}\n", encoding="utf-8")

            self.assertEqual(verifier.qml_files(root), [qml])


if __name__ == "__main__":
    unittest.main()
