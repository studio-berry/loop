#!/usr/bin/env python3
"""Exercise the repository-level Quick shell admission verifier."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-quick-shell-policy.py"


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


if __name__ == "__main__":
    unittest.main()
