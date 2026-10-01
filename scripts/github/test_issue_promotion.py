#!/usr/bin/env python3
"""Tests for scripts/github/issue_promotion.py."""

from __future__ import annotations

import unittest
from unittest.mock import patch
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
        self.issues: dict[int, dict[str, Any]] = {}
        self.stages: dict[int, str | None] = {}
        self.writes: list[tuple[int, str]] = []

    def compare_commits(self, repository: str, before: str, after: str) -> list[dict[str, Any]]:
        return self.commits

    def commit_pull_requests(self, repository: str, sha: str) -> list[dict[str, Any]]:
        return self.commit_pulls.get(sha, [])

    def pull_request(self, repository: str, number: int) -> dict[str, Any]:
        return self.pulls[number]

    def pull_request_commits(self, repository: str, number: int) -> list[dict[str, Any]]:
        return self.pull_commits[number]

    def issue(self, repository: str, number: int) -> dict[str, Any]:
        return self.issues[number]

    def promotion_stage(self, repository: str, number: int) -> str | None:
        return self.stages.get(number)

    def set_promotion_stage(self, repository: str, number: int, stage: str) -> None:
        self.writes.append((number, stage))


class IssuePromotionTests(unittest.TestCase):
    repository = "studio-berry/loop2"

    def test_extracts_multiple_same_repository_refs_and_ignores_external_refs(self) -> None:
        text = (
            "Closes #12, fixes studio-berry/loop2#13, skips other/repo#14, "
            "and see https://github.com/studio-berry/loop2/issues/15."
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

    def test_unstable_expands_squashed_source_commits(self) -> None:
        evidence = module.PromotionEvidence(
            FakeGitHubClient(), self.repository, module.UNSTABLE_BRANCH
        )
        self.assertEqual(evidence.collect("c" * 40, "d" * 40), {101, 102, 103})

    def test_dev_sets_stage_without_changing_issue_state(self) -> None:
        issues = {
            1: {"state": "open"},
            2: {"state": "open"},
            3: {"state": "closed"},
            4: {"state": "open", "pull_request": {}},
        }
        self.assertEqual(
            module.plan_issue_actions(
                module.DEV_BRANCH,
                issues,
                {1: None, 2: "Dev present", 3: None},
            ),
            (
                module.IssueAction(1, "Dev present"),
                module.IssueAction(3, "Dev present"),
            ),
        )

    def test_promotion_is_monotonic_and_stable_never_closes(self) -> None:
        issues = {
            5: {"state": "open"},
            6: {"state": "open"},
            7: {"state": "closed"},
            8: {"state": "open", "pull_request": {}},
        }
        self.assertEqual(
            module.plan_issue_actions(
                module.STABLE_BRANCH,
                issues,
                {5: "Dev present", 6: None, 7: "Stable present"},
            ),
            (
                module.IssueAction(5, "Stable present"),
                module.IssueAction(6, "Stable present"),
            ),
        )
        self.assertEqual(
            module.plan_issue_actions(
                module.DEV_BRANCH, {5: issues[5]}, {5: "Stable present"}
            ),
            (),
        )

    def test_unknown_stage_fails_before_mutation(self) -> None:
        with self.assertRaisesRegex(module.GitHubApiError, "unknown promotion stage"):
            module.plan_issue_actions(
                module.STABLE_BRANCH,
                {1: {"state": "open"}},
                {1: "Needs review"},
            )

    def test_stable_push_sets_field_without_closing_issue(self) -> None:
        client = FakeGitHubClient()
        client.commits = [
            {"sha": "a" * 40, "commit": {"message": "fix: linked work (#15)"}}
        ]
        client.commit_pulls = {"a" * 40: []}
        client.issues = {15: {"state": "open"}}
        client.stages = {15: "Unstable present"}

        self.assertEqual(
            module.process_push(
                client=client,
                repository=self.repository,
                target_branch=module.STABLE_BRANCH,
                before="b" * 40,
                after="c" * 40,
            ),
            0,
        )
        self.assertEqual(client.writes, [(15, "Stable present")])
        self.assertEqual(client.issues[15]["state"], "open")

    def test_failed_field_read_prevents_all_writes(self) -> None:
        client = FakeGitHubClient()
        client.commits = [
            {"sha": "a" * 40, "commit": {"message": "fix: linked work (#15, #16)"}}
        ]
        client.commit_pulls = {"a" * 40: []}
        client.issues = {15: {"state": "open"}, 16: {"state": "open"}}

        def read_stage(repository: str, number: int) -> str | None:
            if number == 16:
                raise module.GitHubApiError("field API unavailable")
            return None

        with patch.object(client, "promotion_stage", side_effect=read_stage):
            with self.assertRaisesRegex(module.GitHubApiError, "field API unavailable"):
                module.process_push(
                    client=client,
                    repository=self.repository,
                    target_branch=module.DEV_BRANCH,
                    before="b" * 40,
                    after="c" * 40,
                )
        self.assertEqual(client.writes, [])

    def test_field_api_uses_additive_endpoint(self) -> None:
        client = module.GitHubClient("fake-token")
        calls: list[tuple[str, str, dict[str, Any] | None]] = []

        def request(method: str, path: str, *, payload: dict[str, Any] | None = None) -> Any:
            calls.append((method, path, payload))
            if method == "GET":
                return [
                    {"issue_field_id": 1, "value": "High"},
                    {
                        "issue_field_id": module.PROMOTION_FIELD_ID,
                        "single_select_option": {"name": "Dev present"},
                    },
                ]
            return {}

        with patch.object(client, "request", side_effect=request):
            self.assertEqual(client.promotion_stage(self.repository, 15), "Dev present")
            client.set_promotion_stage(self.repository, 15, "Unstable present")
        self.assertEqual(calls[-1][0], "POST")
        self.assertEqual(
            calls[-1][2],
            {
                "issue_field_values": [
                    {"field_id": module.PROMOTION_FIELD_ID, "value": "Unstable present"}
                ]
            },
        )


if __name__ == "__main__":
    unittest.main()
