#!/usr/bin/env python3
"""Tests for scripts/github/issue_promotion.py."""

from __future__ import annotations

import unittest
from typing import Any

from scripts.github import issue_promotion as module


class FakeGitHubClient:
    def __init__(self) -> None:
        self.commits = [
            {
                "sha": "a" * 40,
                "commit": {"message": "chore: squash promotion"},
            }
        ]
        self.commit_pulls = {
            "a" * 40: [{"number": 900}],
            "b" * 40: [{"number": 901}],
        }
        self.pulls = {
            900: {
                "merged_at": "2026-09-19T00:00:00Z",
                "title": "release: promote fixes",
                "body": "Closes #101 and fixes #102.",
            },
            901: {
                "merged_at": "2026-09-18T00:00:00Z",
                "title": "fix: linked source (#103)",
                "body": "",
            },
        }
        self.pull_commits = {
            900: [
                {
                    "sha": "b" * 40,
                    "commit": {"message": "fix: source change (#103)"},
                }
            ],
            901: [],
        }

    def compare_commits(self, repository: str, before: str, after: str) -> list[dict[str, Any]]:
        return self.commits

    def commit_pull_requests(self, repository: str, sha: str) -> list[dict[str, Any]]:
        return self.commit_pulls.get(sha, [])

    def pull_request(self, repository: str, number: int) -> dict[str, Any]:
        return self.pulls[number]

    def pull_request_commits(self, repository: str, number: int) -> list[dict[str, Any]]:
        return self.pull_commits[number]


class IssuePromotionTests(unittest.TestCase):
    repository = "studio-berry/loop"

    def test_extracts_multiple_same_repository_refs_and_ignores_external_refs(self) -> None:
        text = (
            "Closes #12, fixes studio-berry/loop#13, skips other/repo#14, "
            "and see https://github.com/studio-berry/loop/issues/15."
        )
        self.assertEqual(
            module.extract_issue_references(text, self.repository), {12, 13, 15}
        )

    def test_body_requires_linking_language(self) -> None:
        self.assertEqual(
            module.extract_pull_request_issue_references(
                "fix: geometry (#21)",
                "Evidence mentions #22.\nCloses #23 and resolves #24.",
                self.repository,
            ),
            {21, 23, 24},
        )

    def test_collects_squash_promotion_and_source_pr_evidence(self) -> None:
        client = FakeGitHubClient()
        evidence = module.PromotionEvidence(client, self.repository, module.STABLE_BRANCH)

        self.assertEqual(
            evidence.collect("c" * 40, "d" * 40),
            {101, 102, 103},
        )

    def test_collects_direct_push_commit_refs_without_a_pull_request(self) -> None:
        client = FakeGitHubClient()
        client.commits = [
            {
                "sha": "c" * 40,
                "commit": {"message": "fix: direct change (#104)\n\nCloses #105"},
            }
        ]
        client.commit_pulls = {"c" * 40: []}

        evidence = module.PromotionEvidence(client, self.repository, module.DEV_BRANCH)

        self.assertEqual(evidence.collect("e" * 40, "f" * 40), {104, 105})

    def test_dev_labels_only_open_unqueued_issues(self) -> None:
        issues = {
            1: {"state": "open", "labels": []},
            2: {
                "state": "open",
                "labels": [{"name": module.QUEUE_LABEL}],
            },
            3: {"state": "closed", "labels": []},
            4: {"state": "open", "labels": [], "pull_request": {}},
        }
        self.assertEqual(
            module.plan_issue_actions(module.DEV_BRANCH, issues),
            (module.IssueAction(1, "label", "linked work reached dev"),),
        )

    def test_stable_closes_only_open_queued_issues(self) -> None:
        issues = {
            5: {
                "state": "open",
                "labels": [{"name": module.QUEUE_LABEL}],
            },
            6: {"state": "open", "labels": []},
            7: {
                "state": "closed",
                "labels": [{"name": module.QUEUE_LABEL}],
            },
            8: {
                "state": "open",
                "labels": [{"name": module.QUEUE_LABEL}],
                "pull_request": {},
            },
        }
        self.assertEqual(
            module.plan_issue_actions(module.STABLE_BRANCH, issues),
            (module.IssueAction(5, "close", "queued work reached stable"),),
        )


if __name__ == "__main__":
    unittest.main()
