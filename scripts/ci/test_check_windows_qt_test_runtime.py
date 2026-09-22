#!/usr/bin/env python3
"""Unit tests for the Windows Qt test-runtime guard.

Each rule is fed in-memory text so a violation can be planted without touching the
tree the guard runs over: the source pair (UnitTests/CMakeLists.txt plus
cmake/LoopTestQtRuntime.cmake) for rule 1, and a CMake cache plus a generated
UnitTests/CTestTestfile.cmake for rule 2. The defect the guard exists to prevent
is described in check_windows_qt_test_runtime.py.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.check_windows_qt_test_runtime import (  # noqa: E402
    INCLUDE_LINE,
    INCLUDING_FILE,
    MODULE_FILE,
    PREPEND,
    QT_LOCATION_MARKER,
    check_build,
    check_source,
    read_text,
    windows_build,
)

INCLUDED = f"add_test(Foo \"foo\")\r\n{INCLUDE_LINE}\r\n"

MODULE = (
    "if(WIN32)\r\n"
    f"    {QT_LOCATION_MARKER}\r\n"
    '    set_tests_properties(Foo PROPERTIES ENVIRONMENT_MODIFICATION "'
    + PREPEND
    + 'C:/Qt-aqt/6.11.1/msvc2022_64/bin")\r\n'
    "endif()\r\n"
)

WINDOWS_CACHE = "CMAKE_CXX_COMPILER:FILEPATH=C:/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe\r\n"
# Visual Studio generator caches omit CMAKE_CXX_COMPILER. This is the shape
# written by windows-latest when cmake is invoked without -G.
VISUAL_STUDIO_CACHE = (
    "CMAKE_CXX_FLAGS:STRING=/DWIN32 /D_WINDOWS /GR /EHsc\r\n"
    "CMAKE_LINKER:FILEPATH=C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/link.exe\r\n"
    "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\r\n"
)
LINUX_CACHE = "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\r\nCMAKE_GENERATOR:INTERNAL=Unix Makefiles\r\n"


def ctestfile(provisioned: str, unprovisioned: str) -> str:
    return (
        f'add_test({provisioned} "x")\r\n'
        f'set_tests_properties({provisioned} PROPERTIES ENVIRONMENT_MODIFICATION "{PREPEND}C:/Qt/bin")\r\n'
        f'add_test({unprovisioned} "x")\r\n'
        f'set_tests_properties({unprovisioned} PROPERTIES _BACKTRACE_TRIPLES "add_test")\r\n'
    )


class QtTestRuntimeSourceTest(unittest.TestCase):
    # -- rule: qt-runtime-include-missing --------------------------------------
    def test_flags_a_missing_include(self) -> None:
        violations = check_source("add_test(Foo \"foo\")\r\n", MODULE)
        self.assertEqual([v.rule for v in violations], ["qt-runtime-include-missing"])
        self.assertEqual(violations[0].path, INCLUDING_FILE)
        self.assertIn("only a shell that already has Qt on PATH", violations[0].format())

    def test_allows_the_real_include(self) -> None:
        self.assertEqual(check_source(INCLUDED, MODULE), [])

    # -- rule: qt-runtime-include-duplicated -----------------------------------
    def test_flags_a_duplicated_include(self) -> None:
        violations = check_source(INCLUDED + INCLUDED, MODULE)
        self.assertEqual([v.rule for v in violations], ["qt-runtime-include-duplicated"])
        # the duplicated include is the second one: lines 1/2 are the first pair
        self.assertEqual(violations[0].line, 4)

    # -- rule: qt-runtime-module-missing ---------------------------------------
    def test_flags_a_missing_module(self) -> None:
        violations = check_source(INCLUDED, None)
        self.assertEqual([v.rule for v in violations], ["qt-runtime-module-missing"])
        self.assertEqual(violations[0].path, MODULE_FILE)

    # -- rule: qt-runtime-platform-guard-missing -------------------------------
    def test_flags_a_module_without_the_windows_guard(self) -> None:
        violations = check_source(INCLUDED, MODULE.replace("if(WIN32)", "if(TRUE)"))
        self.assertEqual([v.rule for v in violations], ["qt-runtime-platform-guard-missing"])

    # -- rule: qt-runtime-prepend-missing --------------------------------------
    def test_flags_a_module_that_does_not_record_the_prepend(self) -> None:
        violations = check_source(INCLUDED, MODULE.replace(PREPEND, "PATH=set:"))
        self.assertEqual([v.rule for v in violations], ["qt-runtime-prepend-missing"])
        self.assertIn(PREPEND, violations[0].format())

    # -- rule: qt-runtime-qt-location-unresolved -------------------------------
    def test_flags_a_module_that_hardcodes_an_imported_location(self) -> None:
        module = MODULE.replace(QT_LOCATION_MARKER, "get_target_property(x Qt6::Core IMPORTED_LOCATION_RELEASE)")
        violations = check_source(INCLUDED, module)
        self.assertEqual([v.rule for v in violations], ["qt-runtime-qt-location-unresolved"])

    # -- the tree this guard ships with ---------------------------------------
    def test_the_real_tree_passes(self) -> None:
        root = Path(__file__).resolve().parents[2]
        violations = check_source(read_text(root / INCLUDING_FILE) or "", read_text(root / MODULE_FILE))
        self.assertEqual(violations, [])


class QtTestRuntimeBuildTest(unittest.TestCase):
    # -- rule: qt-runtime-test-unprovisioned -----------------------------------
    def test_flags_a_test_without_the_prepend(self) -> None:
        violations = check_build(WINDOWS_CACHE, ctestfile("Provisioned", "Bare"))
        self.assertEqual([v.rule for v in violations], ["qt-runtime-test-unprovisioned"])
        self.assertIn("Bare", violations[0].format())
        self.assertNotIn("Provisioned", violations[0].format())

    def test_allows_a_fully_provisioned_build(self) -> None:
        text = (
            'add_test(One "x")\r\n'
            f'set_tests_properties(One PROPERTIES ENVIRONMENT_MODIFICATION "{PREPEND}C:/Qt/bin")\r\n'
        )
        self.assertEqual(check_build(WINDOWS_CACHE, text), [])

    def test_allows_the_pre_3_22_environment_fallback(self) -> None:
        # CMake < 3.22 cannot use ENVIRONMENT_MODIFICATION; the module sets the whole PATH.
        text = (
            'add_test(One "x")\r\n'
            'set_tests_properties(One PROPERTIES ENVIRONMENT "PATH=C:/Qt-aqt/6.11.1/msvc2022_64/bin\\;C:/Windows")\r\n'
        )
        self.assertEqual(check_build(WINDOWS_CACHE, text), [])

    def test_flags_a_build_with_no_tests(self) -> None:
        violations = check_build(WINDOWS_CACHE, "# empty\r\n")
        self.assertEqual([v.rule for v in violations], ["qt-runtime-no-tests"])

    # -- no silent skips: an asserted build must be checkable ------------------
    def test_flags_a_missing_cache(self) -> None:
        violations = check_build(None, "add_test(One \"x\")\r\n")
        self.assertEqual([v.rule for v in violations], ["qt-runtime-build-unreadable"])

    def test_flags_a_missing_generated_test_file(self) -> None:
        violations = check_build(WINDOWS_CACHE, None)
        self.assertEqual([v.rule for v in violations], ["qt-runtime-tests-unreadable"])

    def test_flags_a_non_windows_build(self) -> None:
        violations = check_build(LINUX_CACHE, "add_test(One \"x\")\r\n")
        self.assertEqual([v.rule for v in violations], ["qt-runtime-build-not-windows"])

    def test_windows_build_detection(self) -> None:
        self.assertTrue(windows_build(WINDOWS_CACHE))
        self.assertTrue(windows_build(VISUAL_STUDIO_CACHE))
        self.assertFalse(windows_build(LINUX_CACHE))
        self.assertFalse(windows_build(""))

    def test_visual_studio_generator_cache_is_a_windows_build(self) -> None:
        text = (
            'add_test(One "x")\r\n'
            f'set_tests_properties(One PROPERTIES ENVIRONMENT_MODIFICATION "{PREPEND}C:/Qt/bin")\r\n'
        )
        self.assertEqual(check_build(VISUAL_STUDIO_CACHE, text), [])


if __name__ == "__main__":
    unittest.main()
