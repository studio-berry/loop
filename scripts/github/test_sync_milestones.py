#!/usr/bin/env python3
"""Tests for scripts/github/sync_milestones.py."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.github import sync_milestones as module


class SyncMilestonesTests(unittest.TestCase):
    def test_plan_updates_detects_description_and_state_drift(self) -> None:
        specs = [
            module.MilestoneSpec("0.1.0", "shipped", "closed"),
            module.MilestoneSpec("0.1.1", "living", "open"),
        ]
        remote = [
            module.RemoteMilestone(5, "0.1.0", "old", "open"),
            module.RemoteMilestone(4, "0.1.1", "living", "open"),
        ]

        updates = module.plan_updates(specs, remote)

        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0][0].title, "0.1.0")
        self.assertEqual(updates[0][1].number, 5)

    def test_plan_updates_skips_matching_milestones(self) -> None:
        specs = [module.MilestoneSpec("0.1.4", "agent alpha", "open")]
        remote = [module.RemoteMilestone(7, "0.1.4", "agent alpha", "open")]

        self.assertEqual(module.plan_updates(specs, remote), [])

    def test_load_specs_reads_manifest_and_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            milestones_dir = root / "docs" / "github-milestones"
            milestones_dir.mkdir(parents=True)
            (milestones_dir / "manifest.json").write_text(
                json.dumps(
                    {
                        "milestones": [
                            {
                                "title": "0.1.2",
                                "description_file": "0.1.2.md",
                                "state": "open",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (milestones_dir / "0.1.2.md").write_text("operator experience", encoding="utf-8")

            with mock.patch.object(module, "MILESTONES_DIR", milestones_dir), mock.patch.object(
                module, "MANIFEST_PATH", milestones_dir / "manifest.json"
            ):
                specs = module.load_specs()

        self.assertEqual(len(specs), 1)
        self.assertEqual(specs[0].title, "0.1.2")
        self.assertEqual(specs[0].description, "operator experience")


if __name__ == "__main__":
    unittest.main()
