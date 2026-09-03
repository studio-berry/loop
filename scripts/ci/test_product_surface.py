#!/usr/bin/env python3
"""Regression tests for the authoritative product-surface contract."""

from __future__ import annotations

import copy
import json
import os
import tempfile
import unittest
from pathlib import Path

from scripts.product_surface import (
    PROFILES,
    _install_manifest_entries,
    load_json,
    load_manifest,
    validate_cli,
    validate_install,
    validate_manifest,
    validate_packaging_sources,
    validate_source,
)


ROOT = Path(__file__).resolve().parents[2]


class ProductSurfaceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_manifest(ROOT)

    def test_checked_in_manifest_and_sources_validate_for_both_profiles(self):
        self.assertEqual(validate_manifest(self.manifest, ROOT), [])
        self.assertEqual(load_json(ROOT / "docs/schemas/product-surface.schema.json")["$schema"], "https://json-schema.org/draft/2020-12/schema")
        for profile in PROFILES:
            with self.subTest(profile=profile):
                self.assertEqual(validate_source(ROOT, self.manifest, profile), [])
                self.assertEqual(validate_packaging_sources(ROOT, self.manifest, profile), [])

    def test_open_rows_require_owner_and_follow_up_issue(self):
        manifest = copy.deepcopy(self.manifest)
        row = next(item for item in manifest["surfaces"] if item["id"] == "loop-compare")
        row["owner"] = None
        row["follow_up_issue"] = None
        errors = validate_manifest(manifest)
        self.assertTrue(any("OPEN disposition needs a non-empty owner" in error for error in errors))
        self.assertTrue(any("OPEN disposition needs a positive follow_up_issue" in error for error in errors))

    def test_deleted_surface_cannot_be_present_in_a_profile(self):
        manifest = copy.deepcopy(self.manifest)
        row = next(item for item in manifest["surfaces"] if item["id"] == "loop-viewer")
        row["profiles"]["developer"] = "present"
        errors = validate_manifest(manifest)
        self.assertTrue(any("deleted but not absent from every profile" in error for error in errors))

    def test_unexpected_surface_fields_are_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["surfaces"][0]["hand_listed"] = True
        errors = validate_manifest(manifest)
        self.assertTrue(any("unexpected fields: hand_listed" in error for error in errors))

    def _make_install_tree(self) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        install = Path(temporary.name)
        for relative in ("LoopEditor.exe", "PdfTool.exe", "LoopLibCore.dll", "LoopLibQuick.dll"):
            path = install / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        return install

    def test_stray_first_party_artifact_fails_closed(self):
        install = self._make_install_tree()
        (install / "LoopUnexpected.dll").write_bytes(b"fixture")
        errors = validate_install(install, self.manifest, "loop-release")
        self.assertTrue(any("unmanifested first-party artifact" in error for error in errors))

    def test_forbidden_plugin_artifact_fails_closed(self):
        install = self._make_install_tree()
        (install / "ActionListPlugin.dll").write_bytes(b"fixture")
        errors = validate_install(install, self.manifest, "developer")
        self.assertTrue(any("unmanifested first-party artifact" in error for error in errors))

    def test_nested_qt_deploy_artifacts_are_ignored(self):
        install = self._make_install_tree()
        nested_qml = install / "qml" / "QtQuick" / "VectorImage" / "Helpers" / "qquickvectorimagehelpersplugin.dll"
        nested_qml.parent.mkdir(parents=True, exist_ok=True)
        nested_qml.write_bytes(b"fixture")
        (install / "platforms" / "qwindows.dll").parent.mkdir(parents=True, exist_ok=True)
        (install / "platforms" / "qwindows.dll").write_bytes(b"fixture")
        self.assertEqual(validate_install(install, self.manifest, "loop-release"), [])

    def _write_install_manifest(self, install: Path) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        install_manifest = Path(temporary.name) / "install_manifest.txt"
        install_manifest.write_text(
            "\n".join(str(install / name) for name in ("LoopEditor.exe", "PdfTool.exe", "LoopLibCore.dll", "LoopLibQuick.dll"))
            + "\n",
            encoding="utf-8",
        )
        return install_manifest

    def test_install_manifest_drift_fails_closed(self):
        install = self._make_install_tree()
        install_manifest = self._write_install_manifest(install)
        (install / "LoopUnmanifested.dll").write_bytes(b"fixture")
        errors = validate_install(install, self.manifest, "loop-release", install_manifest)
        self.assertTrue(any("absent from CMake install manifest" in error for error in errors))

    def test_qt_deployment_output_is_not_manifest_drift(self):
        # qt_generate_deploy_qml_app_script and windeployqt copy the Qt closure
        # and write qt.conf outside install(), so install_manifest.txt can never
        # list them.  Only first-party payload is held to the manifest.
        install = self._make_install_tree()
        install_manifest = self._write_install_manifest(install)
        (install / "qt.conf").write_text("[Paths]\n", encoding="utf-8")
        (install / "Qt6Core.dll").write_bytes(b"fixture")
        (install / "libQt6Quick.so.6").write_bytes(b"fixture")
        errors = validate_install(install, self.manifest, "loop-release", install_manifest)
        self.assertEqual([error for error in errors if "absent from CMake install manifest" in error], [])

    def test_install_manifest_missing_file_still_fails_closed(self):
        install = self._make_install_tree()
        install_manifest = self._write_install_manifest(install)
        (install / "LoopLibQuick.dll").unlink()
        errors = validate_install(install, self.manifest, "loop-release", install_manifest)
        self.assertTrue(any("CMake install manifest lists missing files" in error for error in errors))

    def test_install_manifest_preserves_final_symlink_name(self):
        install = self._make_install_tree()
        real = install / "real.dll"
        alias = install / "alias.dll"
        real.write_bytes(b"fixture")
        try:
            os.symlink(real, alias)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symbolic links unavailable: {exc}")

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        install_manifest = Path(temporary.name) / "install_manifest.txt"
        install_manifest.write_text(str(alias) + "\n", encoding="utf-8")
        self.assertEqual(_install_manifest_entries(install_manifest, install), {"alias.dll"})

    def _discovery_file(self, commands: list[dict[str, object]]) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "capabilities.json"
        path.write_text(
            json.dumps(
                {
                    "data": {
                        "build_capabilities": self.manifest["cli"]["required_build_capabilities"],
                        "commands": commands,
                    }
                }
            ),
            encoding="utf-8",
        )
        return path

    def test_cli_inventory_is_derived_and_required_commands_cannot_drift(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["cli"]["command_inventory"]["required_commands"] = ["help", "preflight"]
        discovery = self._discovery_file([{"id": "help"}, {"id": "preflight"}])
        self.assertEqual(validate_cli(manifest, profile="loop-release", discovery_json=discovery), [])

        manifest["cli"]["command_inventory"]["required_commands"] = ["missing"]
        errors = validate_cli(manifest, profile="loop-release", discovery_json=discovery)
        self.assertTrue(any("manifest CLI command is absent" in error for error in errors))

        manifest["cli"]["command_inventory"]["required_commands"] = ["help", "preflight"]
        errors = validate_cli(
            manifest,
            profile="loop-release",
            discovery_json=self._discovery_file([{"id": "help"}, {"id": "preflight"}, {"id": "diagnostics"}]),
        )
        self.assertTrue(any("absent from manifest CLI inventory: diagnostics" in error for error in errors))

    def test_developer_profile_allows_extra_registered_commands(self):
        manifest = copy.deepcopy(self.manifest)
        command_ids = list(manifest["cli"]["command_inventory"]["required_commands"])
        command_ids.extend(["audio-book", "audio-book-voices"])
        discovery = self._discovery_file([{"id": command} for command in sorted(command_ids)])
        self.assertEqual(validate_cli(manifest, profile="developer", discovery_json=discovery), [])

    def test_cli_inventory_matches_pdftool_when_available(self):
        pdf_tool = ROOT / "build" / "usr" / "bin" / "PdfTool"
        if not pdf_tool.is_file():
            self.skipTest("PdfTool is not built in this worktree")
        errors = validate_cli(self.manifest, profile="loop-release", pdf_tool=pdf_tool)
        self.assertEqual(errors, [])

    def test_cli_inventory_must_be_sorted_and_unique(self):
        discovery = self._discovery_file([{"id": "preflight"}, {"id": "help"}, {"id": "help"}])
        errors = validate_cli(self.manifest, profile="loop-release", discovery_json=discovery)
        self.assertTrue(any("duplicate ids" in error for error in errors))
        self.assertTrue(any("not deterministically sorted" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
