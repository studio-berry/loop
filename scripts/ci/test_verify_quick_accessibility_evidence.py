#!/usr/bin/env python3
"""Contract tests for the installed-tree Quick accessibility evidence gate."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts.ci import verify_quick_accessibility_evidence as gate


SHA = "0123456789abcdef0123456789abcdef01234567"


def _record(backend: str, **overrides) -> dict:
    claim = gate.NATIVE_CLAIM if backend == "native" else gate.SOFTWARE_CLAIM
    record = {
        "schema_version": 1,
        "kind": gate.KIND,
        "backend": backend,
        "claim": claim,
        "status": "pass",
        "source_sha": SHA,
        "artifact": {
            "scope": gate.INSTALLED_SCOPE,
            "run_root": "/tmp/qual",
            "install_tree": "/tmp/install",
            "executable": "/tmp/qual/usr/bin/ProductQuickAccessibilitySmoke",
            "executable_sha256": "a" * 64,
            "package": None,
            "package_sha256": None,
        },
        "observed": {
            "graphics_api": "d3d11" if backend == "native" else "software",
            "native_accessibility_backend_active": backend == "native",
            "qpa_platform": "windows" if backend == "native" else "windows",
        },
    }
    record.update(overrides)
    return record


def _native_accessibility_record(**overrides) -> dict:
    record = {
        "schema_version": 1,
        "kind": gate.KIND,
        "backend": "native",
        "claim": gate.NATIVE_ACCESSIBILITY_CLAIM,
        "status": "pass",
        "source_sha": SHA,
        "artifact": {
            "scope": gate.INSTALLED_SCOPE,
            "run_root": "/tmp/qual-uia",
            "install_tree": "/tmp/install",
            "executable": "/tmp/qual-uia/usr/bin/ProductQuickAccessibilitySmoke",
            "executable_sha256": "b" * 64,
            "fixture_sha256": "c" * 64,
            "driver_report": "/tmp/qual-uia/native-uia.json",
        },
        "observed": {
            "platform": "Windows UI Automation",
            "native_accessibility_backend_active": True,
            "graphics_api": "d3d11",
            "observation_count": 42,
        },
    }
    record.update(overrides)
    return record


class QuickAccessibilityEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def _write(self, name: str, record) -> Path:
        path = self.root / name
        path.write_text(json.dumps(record), encoding="utf-8")
        return path

    def _verify(self, native, software, sha=SHA, install_tree="/tmp/install", **kwargs):
        return gate.verify(native, software, sha, install_tree, **kwargs)

    def test_native_and_software_records_pass(self):
        native = self._write("native.json", _record("native"))
        software = self._write("software.json", _record("software"))
        self.assertEqual(self._verify(native, software), [])

    def test_native_accessibility_record_passes_when_required(self):
        native = self._write("native.json", _record("native"))
        software = self._write("software.json", _record("software"))
        uia = self._write("uia.json", _native_accessibility_record())
        errors = self._verify(native, software, native_accessibility_path=uia, require_native_accessibility=True)
        self.assertEqual(errors, [])

    def test_missing_native_record_fails_closed(self):
        software = self._write("software.json", _record("software"))
        errors = self._verify(self.root / "absent-native.json", software)
        self.assertTrue(any("native backend accessibility evidence is missing" in e for e in errors))

    def test_required_native_accessibility_lane_must_be_present(self):
        native = self._write("native.json", _record("native"))
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software, require_native_accessibility=True)
        self.assertTrue(any("native accessibility backend lane is missing" in e for e in errors))

    def test_software_record_cannot_satisfy_the_native_lane(self):
        software = self._write("software.json", _record("software"))
        dup = self._write("native.json", _record("software"))
        errors = self._verify(dup, software)
        self.assertTrue(any("software-only run cannot satisfy the native lane" in e for e in errors))

    def test_software_record_cannot_satisfy_the_native_accessibility_lane(self):
        native = self._write("native.json", _record("native"))
        software = self._write("software.json", _record("software"))
        uia = self._write("uia.json", _record("software"))
        errors = self._verify(native, software, native_accessibility_path=uia, require_native_accessibility=True)
        self.assertTrue(any("cannot satisfy the native lane" in e for e in errors))

    def test_inactive_native_accessibility_backend_is_rejected(self):
        record = _native_accessibility_record()
        record["observed"]["native_accessibility_backend_active"] = False
        uia = self._write("uia.json", record)
        native = self._write("native.json", _record("native"))
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software, native_accessibility_path=uia)
        self.assertTrue(any("native accessibility backend active" in e for e in errors))

    def test_native_accessibility_on_software_rasterizer_is_rejected(self):
        record = _native_accessibility_record()
        record["observed"]["graphics_api"] = "software"
        uia = self._write("uia.json", record)
        native = self._write("native.json", _record("native"))
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software, native_accessibility_path=uia)
        self.assertTrue(any("non-native graphics backend" in e for e in errors))

    def test_build_tree_scope_is_rejected(self):
        record = _record("native")
        record["artifact"]["scope"] = "build-tree"
        native = self._write("native.json", record)
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software)
        self.assertTrue(any("must run against an 'installed-tree'" in e for e in errors))

    def test_unknown_graphics_backend_is_rejected(self):
        record = _record("native")
        record["observed"]["graphics_api"] = "unknown"
        native = self._write("native.json", record)
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software)
        self.assertTrue(any("unknown graphics backend" in e for e in errors))

    def test_native_software_fallback_on_real_platform_is_rejected(self):
        record = _record("native")
        record["observed"]["graphics_api"] = "software"
        record["observed"]["qpa_platform"] = "windows"
        native = self._write("native.json", record)
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software)
        self.assertTrue(any("fell back to the software rasterizer" in e for e in errors))

    def test_source_sha_mismatch_is_rejected(self):
        native = self._write("native.json", _record("native", source_sha="f" * 40))
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software)
        self.assertTrue(any("does not match the qualification SHA" in e for e in errors))

    def test_installed_tree_mismatch_is_rejected(self):
        record = _record("native")
        record["artifact"]["install_tree"] = "/tmp/other-install"
        native = self._write("native.json", record)
        software = self._write("software.json", _record("software"))
        errors = self._verify(native, software)
        self.assertTrue(any("does not match" in e for e in errors))

    def test_software_lane_must_report_software_rasterizer(self):
        record = _record("software")
        record["observed"]["graphics_api"] = "d3d11"
        software = self._write("software.json", record)
        native = self._write("native.json", _record("native"))
        errors = self._verify(native, software)
        self.assertTrue(any("did not report the software rasterizer" in e for e in errors))

    def _package_records(self):
        records = [_record("native"), _record("software"), _native_accessibility_record()]
        for record in records:
            record["artifact"]["package_sha256"] = "d" * 64
            record["artifact"]["executable_sha256"] = "a" * 64
        boundary = {
            "kind": "loop-package-boundary-evidence", "status": "passed", "source_sha": SHA, "platform": "windows",
            "package": {"sha256": "d" * 64}, "forbidden_findings": [],
            "checks": {key: True for key in (
                "all_payload_files_hashed", "all_binary_files_inspected", "target_architecture_matches",
                "qt6widgets_absent", "qt6widgets_surface_absent", "unresolved_non_system_dependencies_absent")},
        }
        return records, boundary

    def _verify_package(self, records, boundary):
        native, software, accessibility = [self._write(f"lane-{index}.json", record)
                                           for index, record in enumerate(records)]
        graph = self._write("boundary.json", boundary)
        return self._verify(native, software, native_accessibility_path=accessibility,
                            require_native_accessibility=True, package_boundary_path=graph)

    def test_linux_atspi_record_is_a_native_accessibility_claim(self):
        records, boundary = self._package_records()
        records[2]["observed"].update(platform="Linux AT-SPI", graphics_api="opengl")
        boundary["platform"] = "linux"
        self.assertEqual(self._verify_package(records, boundary), [])

    def test_other_package_cannot_supply_accessibility_evidence(self):
        records, boundary = self._package_records()
        for index in range(3):
            with self.subTest(lane=index):
                records[index]["artifact"]["package_sha256"] = "e" * 64
                self.assertTrue(any("inspected package digest" in e for e in self._verify_package(records, boundary)))
                records[index]["artifact"]["package_sha256"] = "d" * 64

    def test_widgets_or_uninspected_graph_cannot_qualify(self):
        records, boundary = self._package_records()
        for key in tuple(boundary["checks"]):
            with self.subTest(check=key):
                boundary["checks"][key] = False
                self.assertTrue(any("graph check" in e for e in self._verify_package(records, boundary)))
                del boundary["checks"][key]
                self.assertTrue(any("graph check" in e for e in self._verify_package(records, boundary)))
                boundary["checks"][key] = True

    def test_package_graph_from_another_source_cannot_qualify(self):
        records, boundary = self._package_records()
        boundary["source_sha"] = "f" * 40
        self.assertTrue(any("package boundary source SHA" in e for e in self._verify_package(records, boundary)))

    def test_other_native_client_cannot_qualify_the_package(self):
        records, boundary = self._package_records()
        records[2]["observed"]["platform"] = "Linux AT-SPI"
        self.assertTrue(any("package platform" in e for e in self._verify_package(records, boundary)))

    def test_replaced_probe_between_lanes_is_rejected(self):
        records, boundary = self._package_records()
        records[2]["artifact"]["executable_sha256"] = "b" * 64
        self.assertTrue(any("same qualification executable" in e for e in self._verify_package(records, boundary)))

    def test_malformed_native_observation_returns_an_error(self):
        records, boundary = self._package_records()
        records[2]["observed"] = None
        self.assertTrue(any("observed backend record" in e for e in self._verify_package(records, boundary)))


if __name__ == "__main__":
    unittest.main()
