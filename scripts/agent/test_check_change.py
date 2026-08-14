#!/usr/bin/env python3
"""Focused unit tests for check-change policy logic."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("check-change.py")
SPEC = importlib.util.spec_from_file_location("check_change", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class CheckChangeTests(unittest.TestCase):
    def test_branch_slug_is_stable_and_safe(self) -> None:
        self.assertEqual(MODULE.branch_slug("feature/PDF bleed"), "feature-PDF-bleed")
        self.assertEqual(MODULE.branch_slug("..."), "change")

    def test_parse_name_status_handles_renames(self) -> None:
        raw = b"R100\0old/file.cpp\0new/file.cpp\0A\0changes/feature-x.md\0"
        changes = MODULE.parse_name_status(raw)
        self.assertEqual(changes[0].status, "R")
        self.assertEqual(changes[0].old_path, "old/file.cpp")
        self.assertEqual(changes[0].path, "new/file.cpp")
        self.assertEqual(changes[1].status, "A")

    def test_changelog_requires_all_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "entry.md"
            path.write_text("Category: fixed\nSummary: missing audience\n", encoding="utf-8")
            valid, reason = MODULE.parse_changelog(path, {"fixed"})
        self.assertFalse(valid)
        self.assertIn("Audience", reason)

    def test_changelog_accepts_internal_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "entry.md"
            path.write_text(
                "# Agent policy\nCategory: internal\nAudience: developers\nBreaking-Change: no\nSummary: Add a verification gate.\n",
                encoding="utf-8",
            )
            valid, reason = MODULE.parse_changelog(path, {"internal"})
        self.assertTrue(valid, reason)


if __name__ == "__main__":
    unittest.main()
