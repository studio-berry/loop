from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock
import sys

sys.path.insert(0, str(Path(__file__).parent))
import check_phase5_residue


class Phase5ResidueTests(unittest.TestCase):
    def test_maintained_paths_reject_deleted_surface_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text("add_subdirectory(LoopLibGui)\n", encoding="utf-8")
            with mock.patch.object(check_phase5_residue, "tracked_paths", return_value=["CMakeLists.txt"]):
                findings = check_phase5_residue.violations(root)
        self.assertEqual(len(findings), 1)

    def test_historical_paths_are_not_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "docs" / "adr"
            path.mkdir(parents=True)
            (path / "adr-005.md").write_text("LoopLibGui is historical.\n", encoding="utf-8")
            with mock.patch.object(check_phase5_residue, "tracked_paths", return_value=["docs/adr/adr-005.md"]):
                findings = check_phase5_residue.violations(root)
        self.assertEqual(findings, [])

    def test_current_docs_are_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "docs"
            path.mkdir()
            (path / "REPO_MAP.md").write_text("LoopViewer is gone.\n", encoding="utf-8")
            with mock.patch.object(check_phase5_residue, "tracked_paths", return_value=["docs/REPO_MAP.md"]):
                findings = check_phase5_residue.violations(root)
        self.assertEqual(len(findings), 1)

    def test_generated_agent_adapters_are_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "AGENTS.md").write_text("LoopLibGui is gone.\n", encoding="utf-8")
            with mock.patch.object(check_phase5_residue, "tracked_paths", return_value=["AGENTS.md"]):
                findings = check_phase5_residue.violations(root)
        self.assertEqual(len(findings), 1)

    def test_validation_scripts_are_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "scripts"
            path.mkdir()
            (path / "verify-installed-product-graph.py").write_text(
                "FORBIDDEN = 'LoopViewer'\n", encoding="utf-8"
            )
            with mock.patch.object(
                check_phase5_residue,
                "tracked_paths",
                return_value=["scripts/verify-installed-product-graph.py"],
            ):
                findings = check_phase5_residue.violations(root)
        self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
