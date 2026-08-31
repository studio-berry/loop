"""Unit fixtures for exact-SHA package evidence pairing."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("compare_package_boundary_evidence.py")
SPEC = importlib.util.spec_from_file_location("compare_package_boundary_evidence", MODULE_PATH)
assert SPEC and SPEC.loader
PAIR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PAIR)


def evidence(platform: str, source_sha: str, status: str = "passed") -> dict:
    return {
        "schema_version": 1,
        "kind": "loupe-package-boundary-evidence",
        "platform": platform,
        "source_sha": source_sha,
        "status": status,
        "forbidden_findings": [],
        "checks": {
            "all_payload_files_hashed": True,
            "all_binary_files_inspected": True,
            "target_architecture_matches": True,
            "qt6widgets_absent": True,
            "qt6widgets_surface_absent": True,
            "unresolved_non_system_dependencies_absent": True,
        },
        "package": {
            "name": f"{platform}.package",
            "format": "AppImage" if platform == "linux" else "MSI",
            "sha256": "0" * 64,
            "size": 1,
        },
    }


class PackageEvidencePairTests(unittest.TestCase):
    def test_linux_and_windows_evidence_share_the_expected_sha(self):
        source_sha = "d" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linux = root / "linux.json"
            windows = root / "windows.json"
            linux.write_text(json.dumps(evidence("linux", source_sha)), encoding="utf-8")
            windows.write_text(json.dumps(evidence("windows", source_sha.upper())), encoding="utf-8")
            pair = PAIR.compare(linux, windows, source_sha)
            self.assertEqual(pair["status"], "passed")
            self.assertEqual(pair["source_sha"], source_sha)

    def test_pair_rejects_mismatched_sha_and_failed_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linux = root / "linux.json"
            windows = root / "windows.json"
            linux.write_text(json.dumps(evidence("linux", "e" * 40)), encoding="utf-8")
            windows.write_text(json.dumps(evidence("windows", "f" * 40, "failed")), encoding="utf-8")
            with self.assertRaises(PAIR.PairError):
                PAIR.compare(linux, windows, "e" * 40)


if __name__ == "__main__":
    unittest.main()
