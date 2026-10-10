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

    def test_retired_repository_link_is_rejected(self) -> None:
        findings = check_loop_identity.repository_reference_findings(
            {"README.md": "https://github.com/mberrys/Loop-pdf/releases\n"})
        self.assertEqual(len(findings), 1)

    def test_reset_repository_redirect_is_rejected_in_active_metadata(self) -> None:
        self.assertTrue(check_loop_identity.repository_reference_findings(
            {"workflow.yml": "https://github.com/studio-berry/loop2"}))

    def test_appimage_update_destination_is_checked(self) -> None:
        self.assertTrue(check_loop_identity.repository_reference_findings(
            {"LinuxInstall.yml": "gh-releases-zsync|mberrys|Loop-pdf|latest|"}))

    def test_upgrade_identity_change_requires_migration_proof(self) -> None:
        paths = ("WixInstaller/Product.wxs.in", "AppxManifest.xml.in",
                 "Flatpak/io.github.mberrys.Loop-pdf.json")
        files = {path: (ROOT / path).read_text(encoding="utf-8") for path in paths}
        files[paths[0]] = files[paths[0]].replace(
            "26336d8a-b2e7-44fc-9a73-68aa99900c7a", "00000000-0000-0000-0000-000000000000")
        self.assertTrue(any("migration proof required" in finding
                            for finding in check_loop_identity.persisted_identity_findings(files)))

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
