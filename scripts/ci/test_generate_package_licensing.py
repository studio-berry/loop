"""Unit fixtures for final-artifact SBOM and notices generation."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


COMMON_PATH = Path(__file__).with_name("package_licensing_common.py")
SBOM_PATH = Path(__file__).with_name("generate_package_sbom.py")
NOTICES_PATH = Path(__file__).with_name("generate_package_third_party_notices.py")
COLLECT_PATH = Path(__file__).with_name("collect_package_licensing_evidence.py")


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


COMMON = load_module(COMMON_PATH, "package_licensing_common")
SBOM = load_module(SBOM_PATH, "generate_package_sbom")
NOTICES = load_module(NOTICES_PATH, "generate_package_third_party_notices")
COLLECT = load_module(COLLECT_PATH, "collect_package_licensing_evidence")


def sample_evidence(platform: str) -> dict:
    source_sha = "a" * 40
    return {
        "schema_version": 1,
        "kind": "loop-package-boundary-evidence",
        "source_sha": source_sha,
        "platform": platform,
        "status": "passed",
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
            "sha256": "b" * 64,
            "size": 123,
        },
        "binaries": [
            {
                "path": "usr/bin/LoopEditor" if platform == "linux" else "LoopEditor.exe",
                "format": "ELF" if platform == "linux" else "PE",
                "sha256": "c" * 64,
                "size": 10,
            },
            {
                "path": "usr/lib/libQt6Core.so.6" if platform == "linux" else "Qt6Core.dll",
                "format": "ELF" if platform == "linux" else "PE",
                "sha256": "d" * 64,
                "size": 20,
            },
            {
                "path": "usr/lib/libssl.so.3" if platform == "linux" else "libssl-3-x64.dll",
                "format": "ELF" if platform == "linux" else "PE",
                "sha256": "e" * 64,
                "size": 30,
            },
        ],
    }


class PackageLicensingTests(unittest.TestCase):
    def test_classify_binary_maps_qt_and_openssl(self):
        self.assertEqual(COMMON.classify_binary("usr/lib/libQt6Quick.so.6").name, "Qt 6")
        self.assertEqual(COMMON.classify_binary("libssl-3-x64.dll").name, "OpenSSL")

    def test_sbom_contains_component_packages(self):
        sbom = SBOM.build_sbom(sample_evidence("linux"))
        self.assertEqual(sbom["spdxVersion"], "SPDX-2.3")
        names = {package["name"] for package in sbom["packages"]}
        self.assertIn("Qt 6", names)
        self.assertIn("OpenSSL", names)
        self.assertIn("Loop", names)

    def test_notices_include_source_sha_and_component_sections(self):
        text = NOTICES.build_notices(sample_evidence("windows"))
        self.assertIn("a" * 40, text)
        self.assertIn("Qt 6", text)
        self.assertIn("OpenSSL", text)

    def test_collect_marks_incomplete_without_all_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linux = root / "linux.json"
            windows = root / "windows.json"
            linux.write_text(json.dumps(sample_evidence("linux")), encoding="utf-8")
            windows.write_text(json.dumps(sample_evidence("windows")), encoding="utf-8")
            evidence = COLLECT.collect(linux, windows, "a" * 40, None, None, None, None, None, None, None)
            self.assertEqual(evidence["status"], "incomplete")
            self.assertEqual(evidence["release_gates"]["final_artifact_sbom"], "open")


if __name__ == "__main__":
    unittest.main()
