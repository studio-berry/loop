"""Unit fixtures for the package-boundary inspector."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("inspect_package_dependencies.py")
SPEC = importlib.util.spec_from_file_location("inspect_package_dependencies", MODULE_PATH)
assert SPEC and SPEC.loader
INSPECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECTOR)


class PackageInspectorTests(unittest.TestCase):
    def test_widgets_names_are_forbidden_regardless_of_platform_path(self):
        self.assertTrue(INSPECTOR.is_widgets_name("Qt6Widgets.dll"))
        self.assertTrue(INSPECTOR.is_widgets_name("libQt6Widgets.so.6"))
        self.assertTrue(INSPECTOR.is_widgets_name(r"plugins\platforms\Qt6Widgets.dll"))
        self.assertFalse(INSPECTOR.is_widgets_name("Qt6Core.dll"))
        self.assertTrue(INSPECTOR.is_forbidden_widgets_surface_name("Qt6PrintSupport.dll"))
        self.assertTrue(INSPECTOR.is_forbidden_widgets_surface_name("libQt6QuickWidgets.so.6"))
        self.assertFalse(INSPECTOR.is_forbidden_widgets_surface_name("Qt6Quick.dll"))

    def test_linux_dependency_parsers_capture_direct_and_unresolved_imports(self):
        readelf = """
 0x0000000000000001 (NEEDED)             Shared library: [libLoopLibCore.so]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
"""
        self.assertEqual(
            INSPECTOR.parse_readelf_needed(readelf),
            ["libLoopLibCore.so", "libc.so.6"],
        )
        resolved, unresolved = INSPECTOR.parse_ldd(
            """
libLoopLibCore.so => /payload/libLoopLibCore.so (0x00007f)
libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f)
libMissing.so => not found
linux-vdso.so.1 (0x00007fff)
"""
        )
        self.assertEqual(unresolved, ["libMissing.so"])
        self.assertEqual(resolved[0], {"name": "libLoopLibCore.so", "path": "/payload/libLoopLibCore.so"})

    def test_dumpbin_parser_captures_dependency_names(self):
        dependencies = INSPECTOR.parse_dumpbin_dependents(
            """
Image has the following dependencies:

    Qt6Core.dll
    KERNEL32.dll
    LoopLibCore.dll
"""
        )
        self.assertEqual(dependencies, ["Qt6Core.dll", "KERNEL32.dll", "LoopLibCore.dll"])

    def test_pe_fixture_architecture_is_x64(self):
        image = bytearray(96)
        image[:2] = b"MZ"
        image[0x3C:0x40] = (0x40).to_bytes(4, "little")
        image[0x40:0x44] = b"PE\0\0"
        image[0x44:0x46] = (0x8664).to_bytes(2, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.exe"
            path.write_bytes(image)
            self.assertEqual(INSPECTOR.binary_format(path), "PE")
            self.assertEqual(INSPECTOR.pe_architecture(path), "x64")

    def test_payload_qt6widgets_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "payload"
            root.mkdir()
            (root / "Qt6Widgets.dll").write_bytes(b"forbidden")
            package = Path(directory) / "package.msi"
            package.write_bytes(b"package")
            evidence = INSPECTOR.build_evidence(
                "windows",
                package,
                root,
                "a" * 40,
                [{"name": "dumpbin", "path": "dumpbin.exe", "version": "fixture"}],
                [],
                "x64",
            )
            self.assertEqual(evidence["status"], "failed")
            self.assertFalse(evidence["checks"]["qt6widgets_absent"])
            self.assertFalse(evidence["checks"]["qt6widgets_surface_absent"])
            self.assertTrue(any(item["kind"] == "payload" for item in evidence["forbidden_findings"]))

    def test_printsupport_payload_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "payload"
            root.mkdir()
            (root / "Qt6PrintSupport.dll").write_bytes(b"forbidden")
            package = Path(directory) / "package.msi"
            package.write_bytes(b"package")
            evidence = INSPECTOR.build_evidence(
                "windows",
                package,
                root,
                "f" * 40,
                [{"name": "dumpbin", "path": "dumpbin.exe", "version": "fixture"}],
                [],
                "x64",
            )
            self.assertEqual(evidence["status"], "failed")
            self.assertTrue(evidence["checks"]["qt6widgets_absent"])
            self.assertFalse(evidence["checks"]["qt6widgets_surface_absent"])

    def test_direct_transitive_and_unresolved_dependencies_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "payload"
            root.mkdir()
            (root / "LoopEditor.exe").write_bytes(b"editor")
            package = Path(directory) / "package.msi"
            package.write_bytes(b"package")
            binaries = [
                {
                    "path": "LoopEditor.exe",
                    "format": "PE",
                    "sha256": "0" * 64,
                    "size": 6,
                    "architecture": "x64",
                    "direct_dependencies": ["Qt6Widgets.dll"],
                    "resolved_dependencies": [],
                    "unresolved_dependencies": ["vendor.dll"],
                    "dependency_rows": [],
                    "package_dependency_closure": ["plugins/Qt6Widgets.dll"],
                }
            ]
            evidence = INSPECTOR.build_evidence(
                "windows",
                package,
                root,
                "b" * 40,
                [],
                binaries,
                "x64",
            )
            kinds = {item["kind"] for item in evidence["forbidden_findings"]}
            self.assertEqual(evidence["status"], "failed")
            self.assertIn("dependency", kinds)
            self.assertIn("transitive-dependency", kinds)
            self.assertIn("unresolved-dependency", kinds)
            self.assertFalse(evidence["checks"]["unresolved_non_system_dependencies_absent"])

    def test_elf_direct_and_transitive_widgets_imports_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "payload"
            (root / "usr" / "bin").mkdir(parents=True)
            (root / "usr" / "lib").mkdir(parents=True)
            (root / "usr" / "bin" / "LoopEditor").write_bytes(b"editor")
            (root / "usr" / "lib" / "libQt6Core.so.6").write_bytes(b"core")
            package = Path(directory) / "package.AppImage"
            package.write_bytes(b"package")
            binaries = [
                {
                    "path": "usr/bin/LoopEditor",
                    "format": "ELF",
                    "sha256": "0" * 64,
                    "size": 6,
                    "architecture": "x86-64",
                    "direct_dependencies": ["libQt6Widgets.so.6"],
                    "resolved_dependencies": [],
                    "unresolved_dependencies": [],
                    "dependency_rows": [],
                    "package_dependency_closure": ["usr/lib/libQt6Core.so.6", "usr/lib/libQt6Widgets.so.6"],
                },
                {
                    "path": "usr/lib/libQt6Core.so.6",
                    "format": "ELF",
                    "sha256": "1" * 64,
                    "size": 4,
                    "architecture": "x86-64",
                    "direct_dependencies": [],
                    "resolved_dependencies": [],
                    "unresolved_dependencies": [],
                    "dependency_rows": [],
                    "package_dependency_closure": [],
                },
            ]
            evidence = INSPECTOR.build_evidence(
                "linux",
                package,
                root,
                "e" * 40,
                [],
                binaries,
                "x86-64",
            )
            kinds = {item["kind"] for item in evidence["forbidden_findings"]}
            self.assertEqual(evidence["status"], "failed")
            self.assertIn("dependency", kinds)
            self.assertIn("transitive-dependency", kinds)
            self.assertFalse(evidence["checks"]["qt6widgets_absent"])
            self.assertFalse(evidence["checks"]["qt6widgets_surface_absent"])

    def test_missing_architecture_is_uninspected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "payload"
            root.mkdir()
            (root / "LoopEditor.exe").write_bytes(b"editor")
            package = Path(directory) / "package"
            package.write_bytes(b"package")
            evidence = INSPECTOR.build_evidence(
                "windows",
                package,
                root,
                "c" * 40,
                [],
                [{"path": "LoopEditor.exe", "format": "unknown", "architecture": "unknown"}],
                "x64",
            )
            self.assertEqual(evidence["status"], "failed")
            self.assertFalse(evidence["checks"]["all_binary_files_inspected"])

    def test_missing_inspection_tool_fails_closed(self):
        with self.assertRaises(INSPECTOR.InspectionError):
            INSPECTOR.resolve_required_tool("loop-tool-that-does-not-exist")

    def test_runtime_plugin_candidates_are_inventoried(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "payload"
            plugin_dir = root / "plugins" / "platforms"
            plugin_dir.mkdir(parents=True)
            plugin = plugin_dir / "qoffscreen.dll"
            image = bytearray(96)
            image[:2] = b"MZ"
            image[0x3C:0x40] = (0x40).to_bytes(4, "little")
            image[0x40:0x44] = b"PE\0\0"
            image[0x44:0x46] = (0x8664).to_bytes(2, "little")
            plugin.write_bytes(image)
            package = Path(directory) / "package"
            package.write_bytes(b"package")
            evidence = INSPECTOR.build_evidence(
                "windows",
                package,
                root,
                "d" * 40,
                [],
                [
                    {
                        "path": "plugins/platforms/qoffscreen.dll",
                        "format": "PE",
                        "sha256": "0" * 64,
                        "size": len(image),
                        "architecture": "x64",
                        "direct_dependencies": [],
                        "dependency_rows": [],
                        "package_dependency_closure": [],
                    }
                ],
                "x64",
            )
            self.assertEqual(evidence["status"], "passed")
            self.assertEqual(evidence["runtime_plugin_candidates"][0]["path"], "plugins/platforms/qoffscreen.dll")
            self.assertTrue(evidence["runtime_plugin_candidates"][0]["inspected"])


if __name__ == "__main__":
    unittest.main()
