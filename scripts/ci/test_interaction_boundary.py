#!/usr/bin/env python3
"""Exercise the host-neutral interaction boundary verifier.

The happy path runs the real repository through the CLI. The negative fixtures
matter more: a boundary check that cannot fail is not a boundary check.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-interaction-boundary.py"


def load_verifier():
    spec = importlib.util.spec_from_file_location("verify_interaction_boundary", VERIFIER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verifier = load_verifier()


CLEAN_CMAKE = """
add_library(LibUnderTest STATIC
    sources/thing.cpp
    sources/thing.h
)

target_link_libraries(LibUnderTest PRIVATE LoopLibCore Qt6::Core Qt6::Gui)
target_include_directories(LibUnderTest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sources)
"""

CLEAN_HEADER = """
#ifndef THING_H
#define THING_H

#include <QObject>
#include <QString>

#endif   // THING_H
"""

POLICY = {
    "schema_version": 1,
    "targets": [
        {
            "name": "LibUnderTest",
            "cmake": "LibUnderTest/CMakeLists.txt",
            "sources": "LibUnderTest/sources",
            "linkage": "STATIC",
            "installed": False,
            "allowed_link_targets": ["LoopLibCore", "Qt6::Core", "Qt6::Gui"],
        }
    ],
    "forbidden_includes": ["^QtWidgets(/|$)", "^QWidget$", "^QQuick.*$", "^QSG.*$"],
}


class BoundaryFixture:
    """A minimal repository tree the verifier can be pointed at."""

    def __init__(self, stack: unittest.TestCase, cmake: str = CLEAN_CMAKE, header: str = CLEAN_HEADER):
        self.root = Path(stack.enterContext(tempfile.TemporaryDirectory()))
        sources = self.root / "LibUnderTest" / "sources"
        sources.mkdir(parents=True)
        (self.root / "LibUnderTest" / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
        (sources / "thing.h").write_text(header, encoding="utf-8")
        (sources / "thing.cpp").write_text('#include "thing.h"\n', encoding="utf-8")
        self.policy_path = self.root / "policy.json"
        self.policy_path.write_text(json.dumps(POLICY), encoding="utf-8")

    def run(self) -> list[str]:
        return verifier.validate_repository(self.root, self.policy_path)


class InteractionBoundaryTests(unittest.TestCase):
    def test_repository_boundary_holds(self):
        result = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertIn("interaction boundary ok", result.stdout)

    def test_clean_fixture_passes(self):
        self.assertEqual(BoundaryFixture(self).run(), [])

    def test_linking_widgets_is_rejected(self):
        cmake = CLEAN_CMAKE.replace(
            "PRIVATE LoopLibCore Qt6::Core Qt6::Gui",
            "PRIVATE LoopLibCore Qt6::Core Qt6::Gui Qt6::Widgets",
        )
        errors = BoundaryFixture(self, cmake=cmake).run()
        self.assertTrue(any("Qt6::Widgets" in error for error in errors), errors)

    def test_linking_quick_is_rejected(self):
        cmake = CLEAN_CMAKE.replace("Qt6::Gui)", "Qt6::Gui Qt6::Quick)")
        errors = BoundaryFixture(self, cmake=cmake).run()
        self.assertTrue(any("Qt6::Quick" in error for error in errors), errors)

    def test_shared_linkage_is_rejected(self):
        cmake = CLEAN_CMAKE.replace("LibUnderTest STATIC", "LibUnderTest SHARED")
        errors = BoundaryFixture(self, cmake=cmake).run()
        self.assertTrue(any("add_library" in error for error in errors), errors)

    def test_install_rule_is_rejected(self):
        cmake = CLEAN_CMAKE + "\ninstall(TARGETS LibUnderTest RUNTIME DESTINATION bin)\n"
        errors = BoundaryFixture(self, cmake=cmake).run()
        self.assertTrue(any("install" in error for error in errors), errors)

    def test_export_header_is_rejected(self):
        cmake = CLEAN_CMAKE + "\nGENERATE_EXPORT_HEADER(LibUnderTest)\n"
        errors = BoundaryFixture(self, cmake=cmake).run()
        self.assertTrue(any("export header" in error for error in errors), errors)

    def test_forbidden_include_is_rejected(self):
        header = CLEAN_HEADER.replace("#include <QObject>", "#include <QWidget>")
        errors = BoundaryFixture(self, header=header).run()
        self.assertTrue(any("QWidget" in error for error in errors), errors)

    def test_forbidden_scene_graph_include_is_rejected(self):
        header = CLEAN_HEADER.replace("#include <QObject>", "#include <QSGNode>")
        errors = BoundaryFixture(self, header=header).run()
        self.assertTrue(any("QSGNode" in error for error in errors), errors)

    def test_comments_are_not_violations(self):
        """Prose describing a rule must not be read as breaking it."""
        cmake = (
            "# This target must never link Qt6::Widgets or Qt6::Quick, and has no\n"
            "# install() rule and no GenerateExportHeader call.\n" + CLEAN_CMAKE
        )
        self.assertEqual(BoundaryFixture(self, cmake=cmake).run(), [])

    def test_malformed_policy_is_reported(self):
        fixture = BoundaryFixture(self)
        fixture.policy_path.write_text(json.dumps({"schema_version": 2}), encoding="utf-8")
        with self.assertRaises(verifier.ContractError):
            fixture.run()

    def test_missing_sources_directory_is_reported(self):
        fixture = BoundaryFixture(self)
        policy = json.loads(fixture.policy_path.read_text(encoding="utf-8"))
        policy["targets"][0]["sources"] = "LibUnderTest/absent"
        fixture.policy_path.write_text(json.dumps(policy), encoding="utf-8")
        errors = fixture.run()
        self.assertTrue(any("not a directory" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
