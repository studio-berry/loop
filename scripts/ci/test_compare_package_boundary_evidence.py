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
        "kind": "loop-package-boundary-evidence",
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


def write_operator_dirs(
    root: Path,
    source_sha: str,
    *,
    linux_native_api: str = "opengl",
    windows_native_api: str = "d3d11",
    linux_operator: bool = True,
    windows_editor: bool = True,
) -> tuple[Path, Path]:
    linux_dir = root / "linux"
    windows_dir = root / "windows"
    linux_dir.mkdir()
    windows_dir.mkdir()
    header = f"source_sha={source_sha}\n"
    (linux_dir / "quick-a11y-native.txt").write_text(
        header + f"product-quick-a11y-smoke scene_graph_initialized graphics_api={linux_native_api}\n",
        encoding="utf-8",
    )
    (linux_dir / "quick-a11y-software.txt").write_text(
        header + "product-quick-a11y-smoke scene_graph_initialized graphics_api=software\n",
        encoding="utf-8",
    )
    linux_smoke = header + "OK: LoopEditor native Quick startup graphics_api=opengl\n"
    if linux_operator:
        linux_smoke += "OK: LoopEditor operator launch remained alive for 5 seconds\n"
    (linux_dir / "appimage-smoke.txt").write_text(linux_smoke, encoding="utf-8")
    (windows_dir / "quick-a11y-native.txt").write_text(
        header + f"product-quick-a11y-smoke scene_graph_initialized graphics_api={windows_native_api}\n",
        encoding="utf-8",
    )
    (windows_dir / "quick-a11y-software.txt").write_text(
        header + "product-quick-a11y-smoke scene_graph_initialized graphics_api=software\n",
        encoding="utf-8",
    )
    windows_smoke = header + "OK: LoopEditor native Quick startup graphics_api=d3d11\n"
    if windows_editor:
        windows_smoke += "OK: LoopEditor launched without immediate crash\n"
    (windows_dir / "msi-smoke.txt").write_text(windows_smoke, encoding="utf-8")
    return linux_dir, windows_dir


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

    def test_operator_transcripts_admit_native_graphics_and_editor_launch(self):
        source_sha = "a" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linux = root / "linux.json"
            windows = root / "windows.json"
            linux.write_text(json.dumps(evidence("linux", source_sha)), encoding="utf-8")
            windows.write_text(json.dumps(evidence("windows", source_sha)), encoding="utf-8")
            linux_dir, windows_dir = write_operator_dirs(root, source_sha)
            pair = PAIR.compare(linux, windows, source_sha, linux_dir, windows_dir)
            self.assertEqual(pair["operator_evidence"]["linux_native_graphics_api"], "opengl")
            self.assertEqual(pair["operator_evidence"]["windows_native_graphics_api"], "d3d11")

    def test_operator_transcripts_reject_software_native_a11y(self):
        source_sha = "b" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linux = root / "linux.json"
            windows = root / "windows.json"
            linux.write_text(json.dumps(evidence("linux", source_sha)), encoding="utf-8")
            windows.write_text(json.dumps(evidence("windows", source_sha)), encoding="utf-8")
            linux_dir, windows_dir = write_operator_dirs(root, source_sha, linux_native_api="software")
            with self.assertRaises(PAIR.PairError) as raised:
                PAIR.compare(linux, windows, source_sha, linux_dir, windows_dir)
            self.assertIn("software graphics", str(raised.exception))

    def test_operator_transcripts_reject_skipped_windows_editor_launch(self):
        source_sha = "c" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linux = root / "linux.json"
            windows = root / "windows.json"
            linux.write_text(json.dumps(evidence("linux", source_sha)), encoding="utf-8")
            windows.write_text(json.dumps(evidence("windows", source_sha)), encoding="utf-8")
            linux_dir, windows_dir = write_operator_dirs(root, source_sha, windows_editor=False)
            with self.assertRaises(PAIR.PairError) as raised:
                PAIR.compare(linux, windows, source_sha, linux_dir, windows_dir)
            self.assertIn("skipped Editor launch", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
