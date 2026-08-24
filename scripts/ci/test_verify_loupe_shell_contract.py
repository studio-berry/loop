#!/usr/bin/env python3
"""Negative coverage for scripts/verify-loupe-shell-contract.ps1."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER_PS1 = ROOT / "scripts" / "verify-loupe-shell-contract.ps1"
VERIFIER_PY = ROOT / "scripts" / "verify-loupe-shell-contract.py"
SHELL_PATH = ROOT / "docs" / "loupe-shell.json"


def run_verifier(shell_json: dict) -> subprocess.CompletedProcess[str]:
    SHELL_PATH.write_text(json.dumps(shell_json, indent=2) + "\n", encoding="utf-8")
    if shutil.which("pwsh"):
        return subprocess.run(
            ["pwsh", "-NoProfile", "-File", str(VERIFIER_PS1), "-RepoRoot", str(ROOT)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
    return subprocess.run(
        [sys.executable, str(VERIFIER_PY)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


class VerifyLoupeShellContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.original_shell = SHELL_PATH.read_text(encoding="utf-8")
        self.shell = json.loads(self.original_shell)

    def tearDown(self) -> None:
        SHELL_PATH.write_text(self.original_shell, encoding="utf-8")

    def test_valid_contract_passes(self) -> None:
        result = run_verifier(self.shell)
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)

    def test_duplicate_legacy_surface_fails(self) -> None:
        ledger = list(self.shell["legacy_surface_disposition"])
        ledger.append(dict(ledger[0]))
        broken = dict(self.shell)
        broken["legacy_surface_disposition"] = ledger
        result = run_verifier(broken)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate legacy", (result.stderr + result.stdout).lower())

    def test_missing_plugin_metadata_fails(self) -> None:
        policies = [dict(item) for item in self.shell["plugin_action_policy"]]
        del policies[0]["owner"]
        broken = dict(self.shell)
        broken["plugin_action_policy"] = policies
        result = run_verifier(broken)
        self.assertNotEqual(result.returncode, 0)
        combined = (result.stderr + result.stdout).lower()
        self.assertTrue("missing required field" in combined or "missing owner" in combined)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
