#!/usr/bin/env python3
"""Tests for the active Loop identity contract."""

from __future__ import annotations

import unittest
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.ci import check_loop_identity


class LoopIdentityContractTest(unittest.TestCase):
    def test_repository_contract_passes(self) -> None:
        self.assertEqual(check_loop_identity.contract_findings(), [])

    def test_unallowlisted_legacy_token_is_reported(self) -> None:
        legacy_token = "lo" + "upe"
        findings = check_loop_identity.legacy_token_findings(
            {
                "active.txt": f"name={legacy_token}\n",
                "migration.txt": f"name={legacy_token}\n",
            },
            allowlist={"migration.txt"},
        )
        self.assertEqual(findings, ["active.txt:1: legacy product token"])


if __name__ == "__main__":
    raise SystemExit(unittest.main())
