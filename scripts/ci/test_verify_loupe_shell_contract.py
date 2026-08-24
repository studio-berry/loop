#!/usr/bin/env python3
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "scripts" / "verify-loupe-shell-contract.ps1"


class LoupeShellContractTests(unittest.TestCase):
    def run_verifier(self, shell_contract):
        powershell = shutil.which("powershell") or shutil.which("pwsh")
        if powershell is None:
            self.skipTest("PowerShell is unavailable")
        with tempfile.TemporaryDirectory() as temp_dir:
            shell_path = Path(temp_dir) / "loupe-shell.json"
            shell_path.write_text(json.dumps(shell_contract), encoding="utf-8")
            return subprocess.run(
                [
                    powershell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(VERIFY),
                    "-ShellContractPath",
                    str(shell_path),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )

    def load_contract(self):
        return json.loads((ROOT / "docs" / "loupe-shell.json").read_text(encoding="utf-8"))

    def test_rejects_missing_legacy_ui_entry(self):
        contract = self.load_contract()
        contract["legacy_surface_disposition"].pop()
        result = self.run_verifier(contract)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly 48 entries", result.stdout + result.stderr)

    def test_rejects_missing_plugin_policy(self):
        contract = self.load_contract()
        contract["plugin_action_policy"] = [
            entry
            for entry in contract["plugin_action_policy"]
            if entry["plugin"] != "ActionListPlugin"
        ]
        result = self.run_verifier(contract)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Plugin disposition coverage mismatch", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
