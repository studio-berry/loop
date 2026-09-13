#!/usr/bin/env python3
"""Release-gate contract for GitHub #520 search budget isolation.

Search must use an operation-scoped processing budget instead of the session
budget whose elapsed timer begins at session construction. Exhaustion must be
surfaced as an incomplete search result, not an uncaught exception from the
Quick presentation layer.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

SEARCH_CPP = ROOT / "LoopLibCore/sources/pdfdocumentsearch.cpp"
SEARCH_H = ROOT / "LoopLibCore/sources/pdfdocumentsearch.h"
QUICK_CPP = ROOT / "LoopEditor/quickdocumentmodel.cpp"
TEST_CPP = ROOT / "UnitTests/tst_documentsearchtest.cpp"
CMAKE = ROOT / "UnitTests/CMakeLists.txt"


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
    if "budgetExceeded" not in search_h or "complete" not in search_h:
        failures.append(f"{SEARCH_H.relative_to(ROOT)}: PDFDocumentSearchResult must expose complete/budgetExceeded")
    if "!searchResult.complete" not in quick_cpp:
        failures.append(f"{QUICK_CPP.relative_to(ROOT)}: Quick search must treat incomplete results as failure")

    if not TEST_CPP.is_file():
        failures.append(f"{TEST_CPP.relative_to(ROOT)}: missing focused regression test")
    else:
        test_text = TEST_CPP.read_text(encoding="utf-8")
        if "searchUsesFreshBudgetAfterSessionElapsed" not in test_text:
            failures.append(f"{TEST_CPP.relative_to(ROOT)}: missing fresh-budget regression slot")

    cmake_text = CMAKE.read_text(encoding="utf-8")
    if "UnitTestsDocumentSearch" not in cmake_text:
        failures.append(f"{CMAKE.relative_to(ROOT)}: UnitTestsDocumentSearch must be registered")

    if failures:
        print("ERROR: search budget release gate failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print("Search budget release gate passed: operation-scoped search budget and regression hooks are present.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
