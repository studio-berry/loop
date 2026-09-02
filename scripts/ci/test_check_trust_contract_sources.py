#!/usr/bin/env python3
"""Unit tests for the semantic-trust source audit."""

from __future__ import annotations

import unittest
from pathlib import Path

from check_trust_contract_sources import OVERLAY_FINDINGS_GUARD, relative


class TrustContractSourceTest(unittest.TestCase):
    def test_overlay_guard_is_the_only_findings_empty_exception(self) -> None:
        self.assertEqual(
            OVERLAY_FINDINGS_GUARD,
            "LoopLibInteraction/sources/preflightcontroller.cpp",
        )

    def test_relative_paths_are_posix_paths(self) -> None:
        self.assertEqual(
            relative(Path(__file__).parents[2] / "PdfTool" / "pdftoolpreflight.cpp"),
            "PdfTool/pdftoolpreflight.cpp",
        )


if __name__ == "__main__":
    unittest.main()
