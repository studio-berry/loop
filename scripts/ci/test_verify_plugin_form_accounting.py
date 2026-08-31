#!/usr/bin/env python3
"""Negative coverage for scripts/verify-plugin-form-accounting.py."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-plugin-form-accounting.py"
SHELL_PATH = ROOT / "docs" / "loop-shell.json"


class VerifyPluginFormAccountingTest(unittest.TestCase):
    def test_valid_accounting_passes(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)

    def test_unledgered_repo_ui_fails(self) -> None:
        orphan = ROOT / "CodeGenerator" / "orphan-form.ui"
        original_shell = SHELL_PATH.read_text(encoding="utf-8")
        try:
            orphan.write_text('<?xml version="1.0"?><ui version="4.0"></ui>\n', encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(VERIFIER)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("repo_only", completed.stderr + completed.stdout)
        finally:
            if orphan.is_file():
                orphan.unlink()
            SHELL_PATH.write_text(original_shell, encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(unittest.main())
