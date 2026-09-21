#!/usr/bin/env python3
"""Guard that Windows build-tree tests are handed the Qt runtime path.

Loop's Qt comes from `LOOP_QT_ROOT` (an aqt install), not from vcpkg, so
`VCPKG_APPLOCAL_DEPS` copies no Qt DLL beside the executables in
`build-local/usr/bin`. Until `cmake/LoopTestQtRuntime.cmake` existed, nothing in
the build declared where Qt lives either, so a test started by CTest resolved
Qt6Core.dll/Qt6Quick.dll only when the caller's shell happened to have Qt on
PATH. Without that the Windows loader stopped with the modal "… was not found"
error and the run hung instead of failing (docs/CI.md, "Windows local test
executables"); the editor targets broke first because their import closure is
widest - the five that compile `LoopEditor/*.cpp` add Qt6Quick.

The wiring is one `include()` line in `UnitTests/CMakeLists.txt` plus that
module, and that is exactly the shape a merge drops silently: the file is
appended to by every change that adds a test target, a branch that predates the
fix carries no copy to conflict against, and CI's Windows lane has Qt on PATH
(from install-qt-action), so losing it is invisible there and reappears only as a
developer's blocked local build or test run.

Rule 1 scans the source and runs on every platform, which is what makes a revert
fail the proof on the next change set. Rule 2 verifies the recorded effect in a
configured build (`--build-dir <dir>`): every test in
`UnitTests/CTestTestfile.cmake` must carry the PATH prepend. Passing
`--build-dir` asserts the effect, so a dirty build tree or a build that was not
configured for Windows is a failure rather than a skip.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

INCLUDING_FILE = "UnitTests/CMakeLists.txt"
MODULE_FILE = "cmake/LoopTestQtRuntime.cmake"
INCLUDE_LINE = "include(${CMAKE_SOURCE_DIR}/cmake/LoopTestQtRuntime.cmake)"
PREPEND = "PATH=path_list_prepend:"
QT_LOCATION_MARKER = "get_target_property(_loop_qt_core_configs Qt6::Core IMPORTED_CONFIGURATIONS)"


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    rule: str
    detail: str

    def format(self) -> str:
        return f"{self.path}:{self.line}: {self.rule} {self.detail}"


def line_of(text: str, needle: str) -> int:
    """1-based line number of `needle`, or 1 when it is absent."""
    for index, line in enumerate(text.splitlines(), start=1):
        if needle in line:
            return index
    return 1


def check_source(unit_tests_text: str, module_text: str | None) -> list[Violation]:
    """Rule 1: the source wires the Qt runtime into every test in UnitTests/."""
    violations: list[Violation] = []

    includes = [
        index
        for index, line in enumerate(unit_tests_text.splitlines(), start=1)
        if line.strip() == INCLUDE_LINE
    ]
    if not includes:
        violations.append(
            Violation(
                INCLUDING_FILE,
                1,
                "qt-runtime-include-missing",
                f"expected {INCLUDE_LINE}; without it no test is handed the Qt bin "
                "directory and only a shell that already has Qt on PATH can run one",
            )
        )
    elif len(includes) > 1:
        violations.append(
            Violation(
                INCLUDING_FILE,
                includes[1],
                "qt-runtime-include-duplicated",
                f"the module is included {len(includes)} times; the tests are handed the same "
                "PATH prepend more than once",
            )
        )

    if module_text is None:
        violations.append(
            Violation(
                MODULE_FILE,
                1,
                "qt-runtime-module-missing",
                f"{INCLUDING_FILE} includes this module, so it must exist",
            )
        )
        return violations

    if "if(WIN32)" not in module_text:
        violations.append(
            Violation(
                MODULE_FILE,
                1,
                "qt-runtime-platform-guard-missing",
                "the wiring must stay inside if(WIN32); other platforms resolve Qt through "
                "their own loader",
            )
        )
    if "ENVIRONMENT_MODIFICATION" not in module_text or PREPEND not in module_text:
        violations.append(
            Violation(
                MODULE_FILE,
                1,
                "qt-runtime-prepend-missing",
                f'expected ENVIRONMENT_MODIFICATION "{PREPEND}<qt bin>" so CTest starts a test '
                "with the Qt runtime resolvable",
            )
        )
    if QT_LOCATION_MARKER not in module_text:
        violations.append(
            Violation(
                MODULE_FILE,
                line_of(module_text, "Qt6::Core"),
                "qt-runtime-qt-location-unresolved",
                "the Qt bin directory must come from Qt6::Core's declared "
                "IMPORTED_CONFIGURATIONS; the aqt kit declares no IMPORTED_LOCATION_RELEASE",
            )
        )
    return violations


def windows_build(cache_text: str) -> bool:
    """True when the CMake cache names an MSVC-family compiler."""
    for line in cache_text.splitlines():
        if line.startswith("CMAKE_CXX_COMPILER:"):
            return "cl.exe" in line
    return False


def tests_and_properties(ctestfile_text: str) -> dict[str, list[str]]:
    """Test name -> its set_tests_properties lines, in file order."""
    tests: dict[str, list[str]] = {}
    for line in ctestfile_text.splitlines():
        stripped = line.strip()
        for prefix in ("add_test(", "add_test(NAME "):
            if stripped.startswith(prefix):
                name = stripped[len(prefix) :].split()[0].strip('"')
                tests.setdefault(name, [])
                break
        if stripped.startswith("set_tests_properties("):
            name = stripped[len("set_tests_properties(") :].split()[0]
            tests.setdefault(name, []).append(stripped)
    return tests


def provisioned(property_line: str) -> bool:
    """True when this property line hands the test process the Qt bin directory.

    CMake >= 3.22 records it as `ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:<dir>"`;
    older CMake takes the module's fallback branch, which sets the whole
    `ENVIRONMENT "PATH=<dir>;<inherited>"` instead. Both are the same wiring, so the
    guard accepts either rather than reporting a false violation on an old runner.
    """
    if PREPEND in property_line:
        return True
    return 'ENVIRONMENT "PATH=' in property_line and "bin" in property_line


def check_build(cache_text: str | None, ctestfile_text: str | None) -> list[Violation]:
    """Rule 2: a configured Windows build handed every test the Qt bin directory."""
    if cache_text is None:
        return [
            Violation(
                "CMakeCache.txt",
                1,
                "qt-runtime-build-unreadable",
                "--build-dir was given, so the build's Qt wiring must be verifiable, but "
                "CMakeCache.txt is missing",
            )
        ]
    if not windows_build(cache_text):
        return [
            Violation(
                "CMakeCache.txt",
                1,
                "qt-runtime-build-not-windows",
                "--build-dir names a build that was not configured for Windows, so the effect "
                "cannot be verified there",
            )
        ]
    if ctestfile_text is None:
        return [
            Violation(
                "UnitTests/CTestTestfile.cmake",
                1,
                "qt-runtime-tests-unreadable",
                "--build-dir was given but the generated test file is missing; configure the "
                "build before asserting the effect",
            )
        ]

    tests = tests_and_properties(ctestfile_text)
    if not tests:
        return [
            Violation(
                "UnitTests/CTestTestfile.cmake",
                1,
                "qt-runtime-no-tests",
                "the generated test file declares no tests, so the effect cannot be asserted",
            )
        ]
    missing = sorted(
        name
        for name, properties in tests.items()
        if not any(provisioned(property_line) for property_line in properties)
    )
    if missing:
        return [
            Violation(
                "UnitTests/CTestTestfile.cmake",
                line_of(ctestfile_text, f"add_test({missing[0]}"),
                "qt-runtime-test-unprovisioned",
                f"{len(missing)} of {len(tests)} tests are not handed the Qt runtime: "
                + ", ".join(missing),
            )
        ]
    return []


def read_text(path: Path) -> str | None:
    if not path.is_file():
        return None
    return path.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", help="configured build directory; asserts the recorded effect")
    parser.add_argument("--source-root", default=str(ROOT))
    args = parser.parse_args()

    source_root = Path(args.source_root)
    violations = check_source(
        read_text(source_root / INCLUDING_FILE) or "",
        read_text(source_root / MODULE_FILE),
    )

    if args.build_dir:
        build_dir = Path(args.build_dir)
        violations += check_build(
            read_text(build_dir / "CMakeCache.txt"),
            read_text(build_dir / "UnitTests" / "CTestTestfile.cmake"),
        )

    for violation in violations:
        print(violation.format())
    if violations:
        print(f"qt-runtime guard: {len(violations)} violation(s)")
        return 1
    print("qt-runtime guard: ok (source wiring" + (", build effect" if args.build_dir else "") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
