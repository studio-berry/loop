#!/usr/bin/env python3
"""Release-gate contract for GitHub #520 / #652 search budget isolation.

Search must use an operation-scoped processing budget instead of the session
budget whose elapsed timer begins at session construction. Exhaustion must be
surfaced as an incomplete search result, not an uncaught exception from the
Quick presentation layer.

#652 additionally requires the QuickDocumentModel slots that falsify session
counter reuse (render-operation content) and assert the render-operations
budget kind on exhaustion.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

SEARCH_CPP = ROOT / "LoopLibCore/sources/pdfdocumentsearch.cpp"
SEARCH_H = ROOT / "LoopLibCore/sources/pdfdocumentsearch.h"
QUICK_CPP = ROOT / "LoopEditor/quickdocumentmodel.cpp"
TEST_CPP = ROOT / "UnitTests/tst_documentsearchtest.cpp"
QUICK_TEST_CPP = ROOT / "UnitTests/tst_quickdocumentmodeltest.cpp"
CMAKE = ROOT / "UnitTests/CMakeLists.txt"
PHASE4 = ROOT / "UnitTests/phase4-tests.cmake"


def main() -> int:
    failures: list[str] = []

    search_cpp = SEARCH_CPP.read_text(encoding="utf-8")
    search_h = SEARCH_H.read_text(encoding="utf-8")
    quick_cpp = QUICK_CPP.read_text(encoding="utf-8")

    if "session->getProcessingBudget()" in search_cpp:
        failures.append(f"{SEARCH_CPP.relative_to(ROOT)}: search must not reuse session processing budget")
    if "PDFProcessingBudget searchBudget" not in search_cpp:
        failures.append(f"{SEARCH_CPP.relative_to(ROOT)}: missing operation-scoped search budget")
    if "catch (const PDFBudgetExceededException" not in search_cpp:
        failures.append(f"{SEARCH_CPP.relative_to(ROOT)}: must catch PDFBudgetExceededException")
    if "budgetExceeded" not in search_h or "completed" not in search_h:
        failures.append(f"{SEARCH_H.relative_to(ROOT)}: PDFDocumentSearchResult must expose completed/budgetExceeded")
    if "!searchResult.completed" not in quick_cpp:
        failures.append(f"{QUICK_CPP.relative_to(ROOT)}: Quick search must treat incomplete results as failure")

    if not TEST_CPP.is_file():
        failures.append(f"{TEST_CPP.relative_to(ROOT)}: missing focused regression test")
    else:
        test_text = TEST_CPP.read_text(encoding="utf-8")
        if "searchUsesFreshBudgetAfterSessionElapsed" not in test_text:
            failures.append(f"{TEST_CPP.relative_to(ROOT)}: missing fresh-budget regression slot")

    if not QUICK_TEST_CPP.is_file():
        failures.append(f"{QUICK_TEST_CPP.relative_to(ROOT)}: missing QuickDocumentModel regression test")
    else:
        quick_test = QUICK_TEST_CPP.read_text(encoding="utf-8")
        if "searchesUseAnIndependentProcessingBudget" not in quick_test:
            failures.append(
                f"{QUICK_TEST_CPP.relative_to(ROOT)}: missing independent-budget slot (#652)"
            )
        if "exhaustedSearchReturnsAnIncompleteResult" not in quick_test:
            failures.append(
                f"{QUICK_TEST_CPP.relative_to(ROOT)}: missing exhausted-search slot (#652)"
            )
        if 'QStringLiteral("render-operations")' not in quick_test:
            failures.append(
                f"{QUICK_TEST_CPP.relative_to(ROOT)}: exhausted slot must assert render-operations kind"
            )
        if 'QByteArray content("q\\nQ\\n")' not in quick_test:
            failures.append(
                f"{QUICK_TEST_CPP.relative_to(ROOT)}: independent-budget slot must use render-charging content"
            )

    cmake_text = CMAKE.read_text(encoding="utf-8")
    phase4_text = PHASE4.read_text(encoding="utf-8") if PHASE4.is_file() else ""
    if "UnitTestsDocumentSearch" not in cmake_text and "UnitTestsDocumentSearch" not in phase4_text:
        failures.append("UnitTests: UnitTestsDocumentSearch must be registered")
    if "UnitTestsQuickDocumentModel" not in cmake_text and "UnitTestsQuickDocumentModel" not in phase4_text:
        failures.append("UnitTests: UnitTestsQuickDocumentModel must be registered")

    if failures:
        print("ERROR: search budget release gate failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(
        "Search budget release gate passed: operation-scoped search budget, "
        "DocumentSearch + QuickDocumentModel regression hooks are present."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
