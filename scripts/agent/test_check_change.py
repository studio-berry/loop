#!/usr/bin/env python3
"""Focused unit tests for check-change policy logic."""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).with_name("check-change.py")
SPEC = importlib.util.spec_from_file_location("check_change", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

POLICY_BRANCHES = {
    "default": "stable",
    "integration": "dev",
    "release": "stable",
    "protected": ["dev", "stable"],
}


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

    def test_skip_changelog_on_integration_branch(self) -> None:
        policy = {"branches": POLICY_BRANCHES}
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": ""}, clear=False):
            self.assertEqual(MODULE.skip_changelog_reason("dev", policy, False), "integration branch")
            self.assertEqual(MODULE.skip_changelog_reason("stable", policy, False), "integration branch")
            self.assertIsNone(MODULE.skip_changelog_reason("cursor/foo-0158", policy, False))
            self.assertEqual(MODULE.skip_changelog_reason("cursor/foo-0158", policy, True), "non-PR event")

    def test_skip_changelog_on_non_pr_github_event(self) -> None:
        policy = {"branches": POLICY_BRANCHES}
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": "push"}, clear=False):
            self.assertEqual(MODULE.skip_changelog_reason("cursor/foo-0158", policy, False), "non-PR event")
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": "pull_request"}, clear=False):
            self.assertIsNone(MODULE.skip_changelog_reason("cursor/foo-0158", policy, False))

    def test_check_changelog_demands_dev_fragment_when_branch_is_dev(self) -> None:
        policy = {"changelog": {"directory": "changes", "categories": ["fixed"]}}
        changes = [MODULE.Change("A", "changes/cursor-foo.md")]
        evidence = MODULE.check_changelog(changes, policy, "dev")
        self.assertEqual(evidence.result, "fail")
        self.assertIn("changes/dev.md", evidence.reason or "")

    def test_check_changelog_allows_stacked_topic_fragments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            changes_dir = root / "changes"
            changes_dir.mkdir()
            parent = changes_dir / "cursor-wave-b.md"
            child = changes_dir / "cursor-wave-c.md"
            body = (
                "Category: fixed\nAudience: developers\nBreaking-Change: no\nSummary: Wave fragment.\n"
            )
            parent.write_text(body, encoding="utf-8")
            child.write_text(body, encoding="utf-8")
            policy = {"changelog": {"directory": "changes", "categories": ["fixed"]}}
            changes = [
                MODULE.Change("A", "changes/cursor-wave-b.md"),
                MODULE.Change("A", "changes/cursor-wave-c.md"),
            ]
            with patch.object(MODULE, "ROOT", root):
                evidence = MODULE.check_changelog(changes, policy, "cursor/wave-c")
        self.assertEqual(evidence.result, "pass")

    def test_changelog_evidence_still_required_on_topic_branch(self) -> None:
        policy = {
            "branches": POLICY_BRANCHES,
            "changelog": {"directory": "changes", "categories": ["fixed"]},
        }
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": "pull_request"}, clear=False):
            evidence = MODULE.changelog_evidence(
                [MODULE.Change("A", "changes/other.md")],
                policy,
                "cursor/foo-0158",
                skip=False,
            )
        self.assertEqual(evidence.result, "fail")
        self.assertIn("changes/cursor-foo-0158.md", evidence.reason or "")

    def test_changelog_evidence_skips_topic_fragment_after_merge_to_dev(self) -> None:
        policy = {
            "branches": POLICY_BRANCHES,
            "changelog": {"directory": "changes", "categories": ["fixed"]},
        }
        changes = [MODULE.Change("A", "changes/cursor-foo.md")]
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": ""}, clear=False):
            evidence = MODULE.changelog_evidence(changes, policy, "dev", skip=False)
        self.assertEqual(evidence.result, "not-applicable")
        self.assertEqual(evidence.reason, "integration branch")

    def test_parse_name_status_deleted_is_excluded_from_format(self) -> None:
        raw = b"D\0Pdf4QtLibCore/sources/gone.cpp\0M\0Pdf4QtLibCore/sources/keep.cpp\0"
        changes = MODULE.parse_name_status(raw)
        self.assertEqual(changes[0].status, "D")
        self.assertEqual(MODULE.format_sources(changes), ["Pdf4QtLibCore/sources/keep.cpp"])

    def test_format_sources_excludes_deleted_paths(self) -> None:
        changes = [
            MODULE.Change("D", "Pdf4QtLibCore/sources/old.cpp"),
            MODULE.Change("M", "Pdf4QtLibCore/sources/keep.cpp"),
            MODULE.Change("A", "Pdf4QtLibCore/sources/new.h"),
            MODULE.Change("R", "Pdf4QtLibCore/sources/renamed.cpp", "Pdf4QtLibCore/sources/was.cpp"),
            MODULE.Change("A", "changes/foo.md"),
        ]
        self.assertEqual(
            MODULE.format_sources(changes),
            [
                "Pdf4QtLibCore/sources/keep.cpp",
                "Pdf4QtLibCore/sources/new.h",
                "Pdf4QtLibCore/sources/renamed.cpp",
            ],
        )

    def test_classify_still_uses_deleted_paths(self) -> None:
        policy = {
            "module_boundaries": {
                "core": {"paths": ["Pdf4QtLibCore/**"], "targets": ["Pdf4QtLibCore"], "tests": []},
            }
        }
        changes = [MODULE.Change("D", "Pdf4QtLibCore/sources/gone.cpp")]
        self.assertEqual(MODULE.classify(changes, policy), ["core"])


if __name__ == "__main__":
    unittest.main()
