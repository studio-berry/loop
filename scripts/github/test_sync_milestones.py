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

        creates, updates = module.plan_updates(specs, remote)

        self.assertEqual(creates, [])
        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0][0].title, "0.1.0")
        self.assertEqual(updates[0][1].number, 5)

    def test_plan_updates_creates_missing_milestones(self) -> None:
        specs = [
            module.MilestoneSpec("0.2.0", "operator experience", "open"),
            module.MilestoneSpec("0.1.1", "living", "open"),
        ]
        remote = [module.RemoteMilestone(4, "0.1.1", "living", "open")]

        creates, updates = module.plan_updates(specs, remote)

        self.assertEqual(len(creates), 1)
        self.assertEqual(creates[0].title, "0.2.0")
        self.assertEqual(updates, [])

    def test_plan_updates_skips_matching_milestones(self) -> None:
        specs = [module.MilestoneSpec("0.4.0", "agent alpha", "open")]
        remote = [module.RemoteMilestone(7, "0.4.0", "agent alpha", "open")]

        creates, updates = module.plan_updates(specs, remote)

        self.assertEqual(creates, [])
        self.assertEqual(updates, [])

    def test_plan_retirements_closes_superseded_titles(self) -> None:
        retire = [module.RetireSpec("0.1.2", "retired")]
        remote = [module.RemoteMilestone(3, "0.1.2", "old text", "open")]

        retirements = module.plan_retirements(retire, remote)

        self.assertEqual(len(retirements), 1)
        self.assertEqual(retirements[0][0].title, "0.1.2")

    def test_plan_retirements_skips_already_retired(self) -> None:
        retire = [module.RetireSpec("0.1.2", "retired")]
        remote = [module.RemoteMilestone(3, "0.1.2", "retired", "closed")]

        self.assertEqual(module.plan_retirements(retire, remote), [])

    def test_load_manifest_reads_manifest_and_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            milestones_dir = root / "docs" / "github-milestones"
            milestones_dir.mkdir(parents=True)
            (milestones_dir / "manifest.json").write_text(
                json.dumps(
                    {
                        "milestones": [
                            {
                                "title": "0.2.0",
                                "description_file": "0.2.0.md",
                                "state": "open",
                            }
                        ],
                        "retire": [
                            {
                                "title": "0.1.2",
                                "description": "retired",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            (milestones_dir / "0.2.0.md").write_text("operator experience", encoding="utf-8")

            with mock.patch.object(module, "MILESTONES_DIR", milestones_dir), mock.patch.object(
                module, "MANIFEST_PATH", milestones_dir / "manifest.json"
            ):
                specs, retire = module.load_manifest()

        self.assertEqual(len(specs), 1)
        self.assertEqual(specs[0].title, "0.2.0")
        self.assertEqual(specs[0].description, "operator experience")
        self.assertEqual(len(retire), 1)
        self.assertEqual(retire[0].title, "0.1.2")

    @mock.patch.object(module, "gh_api")
    def test_list_remote_milestones_requests_all_pages(self, gh_api: mock.Mock) -> None:
        gh_api.return_value = [
            {"number": 1, "title": "0.1.0", "description": "old", "state": "closed"},
            {"number": 101, "title": "0.2.0", "description": "new", "state": "open"},
        ]

        milestones = module.list_remote_milestones()

        gh_api.assert_called_once_with(
            "GET",
            "repos/studio-berry/loupe/milestones?state=all&per_page=100",
            paginate=True,
        )
        self.assertEqual([item.number for item in milestones], [1, 101])


if __name__ == "__main__":
    unittest.main()
