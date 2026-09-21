# Provision the Qt runtime for Windows build-tree tests and dev scripts

Category: fixed
Audience: developers
Breaking-Change: no
Summary: Stop the Windows loader failures that blocked building and testing the
editor locally. CTest records the configured Qt bin directory on every test the
unit-test directory defines, so build-tree test executables resolve Qt6Core.dll
and Qt6Quick.dll without a primed shell PATH; add scripts/dev-env.ps1 as the
Windows counterpart of scripts/dev-env.sh; make the Quick smoke scripts provision
the same Qt runtime themselves; and replace docs/CI.md's retired manual
windeployqt advice with the supported flow. The wiring is cmake/LoopTestQtRuntime.cmake
behind one include line at the end of UnitTests/CMakeLists.txt, and
scripts/ci/check_windows_qt_test_runtime.py asserts it on every proof run and as a
Windows CI step, so a merge that drops it fails loudly instead of silently
returning the modal loader failure. Also repair the UnitTestsQuickDocumentModel
test's PDFStream construction, which no longer compiled against the core
constructor and so had been hiding two failing search-budget assertions.
