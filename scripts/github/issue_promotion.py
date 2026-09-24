#!/usr/bin/env python3
"""Track the highest promotion branch containing linked issue work.

The workflow runs on pushes to dev, unstable, and stable. Evidence is collected
from the exact ``before...after`` push range, merged pull-request metadata,
and source commits for promotion PRs. Mutations happen after all issue and
field reads complete, so a partial API read cannot cause a partial promotion
claim. Issue state is never changed by this workflow.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


PROMOTION_FIELD_ID = 47367010
DEV_BRANCH = "dev"
UNSTABLE_BRANCH = "unstable"
STABLE_BRANCH = "stable"
STAGE_BY_BRANCH = {
    DEV_BRANCH: "Dev present",
    UNSTABLE_BRANCH: "Unstable present",
    STABLE_BRANCH: "Stable present",
}
STAGE_RANK = {stage: rank for rank, stage in enumerate(STAGE_BY_BRANCH.values())}
SUPPORTED_BRANCHES = frozenset(STAGE_BY_BRANCH)
ZERO_SHA = "0" * 40
FULL_SHA = re.compile(r"^[0-9a-f]{40}$", re.IGNORECASE)

_ISSUE_URL = re.compile(
    r"https?://github\.com/(?P<repository>[\w.-]+/[\w.-]+)/issues/"
    r"(?P<number>[1-9]\d{0,8})(?=$|[/?#\s.,;:!?])",
    re.IGNORECASE,
)
_QUALIFIED_ISSUE = re.compile(
    r"(?<![\w.-])(?P<repository>[\w.-]+/[\w.-]+)#"
    r"(?P<number>[1-9]\d{0,8})\b",
    re.IGNORECASE,
)
_BARE_ISSUE = re.compile(r"(?<![\w/.-])#(?P<number>[1-9]\d{0,8})\b")
_LINKING_LANGUAGE = re.compile(
    r"\b(?:"
    r"close[sd]?|fix(?:e[sd])?|resolve[sd]?|implement(?:s|ed)?|"
    r"address(?:e[sd])?|relat(?:e|es|ed)?(?:\s+to)?|"
    r"refer(?:s|red)?|ref(?:s)?|tracked\s+by|part\s+of"
    r")\b",
    re.IGNORECASE,
)


class GitHubApiError(RuntimeError):
    """An API request failed before issue state could be safely updated."""

    def __init__(self, message: str, *, status_code: int | None = None) -> None:
        super().__init__(message)
        self.status_code = status_code


def _same_repository(candidate: str, repository: str) -> bool:
    return candidate.casefold() == repository.casefold()


def extract_issue_references(text: str, repository: str) -> set[int]:
    """Return same-repository issue references from arbitrary text.

    Qualified references and issue URLs are accepted only for the configured
    repository. A bare ``#123`` is interpreted as a reference in that same
    repository, matching GitHub's normal issue-linking convention.
    """

    numbers: set[int] = set()
    for pattern in (_ISSUE_URL, _QUALIFIED_ISSUE):
        for match in pattern.finditer(text or ""):
            if _same_repository(match.group("repository"), repository):
                numbers.add(int(match.group("number")))

    for match in _BARE_ISSUE.finditer(text or ""):
        numbers.add(int(match.group("number")))
    return numbers


def _extract_linked_body_references(text: str, repository: str) -> set[int]:
    numbers: set[int] = set()
    for line in (text or "").splitlines():
        if _LINKING_LANGUAGE.search(line):
            numbers.update(extract_issue_references(line, repository))
    return numbers


def extract_commit_issue_references(message: str, repository: str) -> set[int]:
    """Extract refs tied to a commit subject or explicit body link."""

    lines = (message or "").splitlines()
    if not lines:
        return set()
    numbers = extract_issue_references(lines[0], repository)
    numbers.update(_extract_linked_body_references("\n".join(lines[1:]), repository))
    return numbers


def extract_pull_request_issue_references(
    title: str, body: str | None, repository: str
) -> set[int]:
    """Extract refs from a PR title and explicit linking lines in its body."""

    numbers = extract_issue_references(title or "", repository)
    numbers.update(_extract_linked_body_references(body or "", repository))
    return numbers


class GitHubClient:
    """Small REST client with pagination kept explicit for testability."""

    def __init__(self, token: str, *, api_url: str = "https://api.github.com") -> None:
        self.api_url = api_url.rstrip("/")
        self._headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "User-Agent": "loop-issue-promotion",
            "X-GitHub-Api-Version": "2026-03-10",
        }

    def request(
        self,
        method: str,
        path: str,
        *,
        query: Mapping[str, Any] | None = None,
        payload: Mapping[str, Any] | None = None,
    ) -> Any:
        url = f"{self.api_url}/{path.lstrip('/')}"
        if query:
            url = f"{url}?{urlencode({key: str(value) for key, value in query.items()})}"
        data = None
        headers = dict(self._headers)
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = Request(url, data=data, headers=headers, method=method)
        try:
            with urlopen(request, timeout=30) as response:
                raw = response.read()
        except HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace").strip()
            raise GitHubApiError(
                f"GitHub API {method} {path} failed with HTTP {exc.code}: {detail}",
                status_code=exc.code,
            ) from exc
        except URLError as exc:
            raise GitHubApiError(f"GitHub API {method} {path} failed: {exc.reason}") from exc

        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise GitHubApiError(f"GitHub API {method} {path} returned invalid JSON") from exc

    def _paged_list(self, path: str) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        page = 1
        per_page = 100
        while True:
            response = self.request(
                "GET", path, query={"page": page, "per_page": per_page}
            )
            if not isinstance(response, list):
                raise GitHubApiError(f"GitHub API {path} returned a non-list response")
            items.extend(item for item in response if isinstance(item, dict))
            if len(response) < per_page:
                return items
            page += 1

    def compare_commits(self, repository: str, before: str, after: str) -> list[dict[str, Any]]:
        """Return every commit in a forward push range, failing closed if truncated."""

        if before == after or before == ZERO_SHA:
            return []
        commits: list[dict[str, Any]] = []
        page = 1
        per_page = 100
        total: int | None = None
        while True:
            response = self.request(
                "GET",
                f"repos/{repository}/compare/{before}...{after}",
                query={"page": page, "per_page": per_page},
            )
            if not isinstance(response, dict):
                raise GitHubApiError("GitHub compare API returned a non-object response")
            status = response.get("status")
            if status not in (None, "ahead", "identical"):
                raise GitHubApiError(
                    f"push range {before[:12]}...{after[:12]} is not a forward comparison "
                    f"(status={status!r}); no issue state was changed"
                )
            page_commits = response.get("commits", [])
            if not isinstance(page_commits, list):
                raise GitHubApiError("GitHub compare API returned invalid commits")
            commits.extend(item for item in page_commits if isinstance(item, dict))
            if total is None and isinstance(response.get("total_commits"), int):
                total = response["total_commits"]
            if total is not None and len(commits) >= total:
                break
            if not page_commits:
                break
            page += 1

        if total is not None and len(commits) != total:
            raise GitHubApiError(
                f"GitHub compare API returned {len(commits)} of {total} commits; "
                "no issue state was changed"
            )
        return commits

    def commit_pull_requests(self, repository: str, sha: str) -> list[dict[str, Any]]:
        return self._paged_list(f"repos/{repository}/commits/{sha}/pulls")

    def pull_request(self, repository: str, number: int) -> dict[str, Any]:
        response = self.request("GET", f"repos/{repository}/pulls/{number}")
        if not isinstance(response, dict):
            raise GitHubApiError(f"pull request #{number} returned a non-object response")
        return response

    def pull_request_commits(self, repository: str, number: int) -> list[dict[str, Any]]:
        return self._paged_list(f"repos/{repository}/pulls/{number}/commits")

    def issue(self, repository: str, number: int) -> dict[str, Any]:
        response = self.request("GET", f"repos/{repository}/issues/{number}")
        if not isinstance(response, dict):
            raise GitHubApiError(f"issue #{number} returned a non-object response")
        return response

    def promotion_stage(self, repository: str, number: int) -> str | None:
        path = f"repos/{repository}/issues/{number}/issue-field-values"
        values = self.request("GET", path)
        if not isinstance(values, list):
            raise GitHubApiError(f"issue #{number} field values returned a non-list response")
        matches = [
            value for value in values
            if isinstance(value, dict) and value.get("issue_field_id") == PROMOTION_FIELD_ID
        ]
        if not matches:
            return None
        if len(matches) != 1:
            raise GitHubApiError(f"issue #{number} has duplicate promotion field values")
        option = matches[0].get("single_select_option")
        if not isinstance(option, dict) or not isinstance(option.get("name"), str):
            raise GitHubApiError(f"issue #{number} has an invalid promotion field value")
        return option["name"]

    def set_promotion_stage(self, repository: str, number: int, stage: str) -> None:
        self.request(
            "POST",
            f"repos/{repository}/issues/{number}/issue-field-values",
            payload={"issue_field_values": [{"field_id": PROMOTION_FIELD_ID, "value": stage}]},
        )


@dataclass
class PromotionEvidence:
    """Collect issue refs without mutating GitHub."""

    client: Any
    repository: str
    target_branch: str
    issue_numbers: set[int] = field(default_factory=set)
    _visited_commits: set[str] = field(default_factory=set)
    _visited_pull_requests: set[int] = field(default_factory=set)

    def collect(self, before: str, after: str) -> set[int]:
        for commit in self.client.compare_commits(self.repository, before, after):
            self._collect_commit(commit)
        return set(self.issue_numbers)

    def _collect_commit(self, commit: Mapping[str, Any]) -> None:
        sha = commit.get("sha")
        if not isinstance(sha, str) or not FULL_SHA.fullmatch(sha):
            raise GitHubApiError("GitHub compare API returned a commit without a valid SHA")
        if sha in self._visited_commits:
            return
        self._visited_commits.add(sha)
        commit_data = commit.get("commit")
        message = commit_data.get("message", "") if isinstance(commit_data, dict) else ""
        self.issue_numbers.update(extract_commit_issue_references(message, self.repository))
        for pull in self.client.commit_pull_requests(self.repository, sha):
            number = pull.get("number")
            if isinstance(number, int):
                self._collect_pull_request(number)

    def _collect_pull_request(self, number: int) -> None:
        if number in self._visited_pull_requests:
            return
        pull = self.client.pull_request(self.repository, number)
        if pull.get("merged_at") is None:
            return
        self._visited_pull_requests.add(number)

        pull_refs = extract_pull_request_issue_references(
            str(pull.get("title", "")), pull.get("body"), self.repository
        )
        self.issue_numbers.update(pull_refs)

        # A promotion may be merged as a squash commit. Its push range
        # then contains only the new squash SHA, so inspect the promotion PR's
        # source commits and their merged topic PRs as well. A dev squash PR
        # is expanded only when its title/body had no usable issue link.
        expand_source = self.target_branch != DEV_BRANCH or not pull_refs
        if not expand_source:
            return
        for commit in self.client.pull_request_commits(self.repository, number):
            self._collect_commit(commit)


@dataclass(frozen=True)
class IssueAction:
    number: int
    stage: str


def plan_issue_actions(
    target_branch: str,
    issues: Mapping[int, Mapping[str, Any]],
    current_stages: Mapping[int, str | None],
) -> tuple[IssueAction, ...]:
    """Plan monotonic field updates without changing issue state."""

    if target_branch not in SUPPORTED_BRANCHES:
        raise ValueError(f"unsupported promotion branch: {target_branch!r}")

    actions: list[IssueAction] = []
    target_stage = STAGE_BY_BRANCH[target_branch]
    for number in sorted(issues):
        issue = issues[number]
        if "pull_request" in issue:
            continue
        current_stage = current_stages[number]
        if current_stage is not None and current_stage not in STAGE_RANK:
            raise GitHubApiError(
                f"issue #{number} has unknown promotion stage {current_stage!r}"
            )
        if current_stage is None or STAGE_RANK[current_stage] < STAGE_RANK[target_stage]:
            actions.append(IssueAction(number, target_stage))
    return tuple(actions)


def apply_issue_actions(
    client: GitHubClient,
    repository: str,
    actions: Iterable[IssueAction],
) -> None:
    for action in actions:
        client.set_promotion_stage(repository, action.number, action.stage)
        print(f"issue #{action.number}: promotion stage set to {action.stage!r}")


def process_push(
    *,
    client: GitHubClient,
    repository: str,
    target_branch: str,
    before: str,
    after: str,
) -> int:
    if target_branch not in SUPPORTED_BRANCHES:
        raise ValueError(f"unsupported promotion branch: {target_branch!r}")
    if not FULL_SHA.fullmatch(before) or not FULL_SHA.fullmatch(after):
        raise ValueError("push event is missing full before/after commit SHAs")
    if before == ZERO_SHA:
        print("Initial branch creation has no comparable promotion range; nothing to do.")
        return 0
    if before == after:
        print("Push did not advance the branch; nothing to do.")
        return 0

    evidence = PromotionEvidence(client, repository, target_branch)
    numbers = evidence.collect(before, after)
    if not numbers:
        print(f"No same-repository issue links found in {before[:12]}...{after[:12]}.")
        return 0

    issues: dict[int, Mapping[str, Any]] = {}
    current_stages: dict[int, str | None] = {}
    for number in sorted(numbers):
        try:
            issues[number] = client.issue(repository, number)
        except GitHubApiError as exc:
            if exc.status_code == 404:
                print(f"issue #{number}: not found; skipped", file=sys.stderr)
                continue
            raise
        if "pull_request" not in issues[number]:
            current_stages[number] = client.promotion_stage(repository, number)

    actions = plan_issue_actions(target_branch, issues, current_stages)
    print(
        f"Found {len(numbers)} issue link(s); planned {len(actions)} action(s) "
        f"for {target_branch}."
    )
    apply_issue_actions(client, repository, actions)
    return 0


def _load_event(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("GitHub event payload must be an object")
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    event_path_value = os.environ.get("GITHUB_EVENT_PATH")
    parser.add_argument(
        "--event-path",
        type=Path,
        default=Path(event_path_value) if event_path_value else None,
        help="GitHub push event JSON (defaults to GITHUB_EVENT_PATH)",
    )
    args = parser.parse_args(argv)
    if args.event_path is None or not args.event_path.is_file():
        raise ValueError("GITHUB_EVENT_PATH is required")

    event_name = os.environ.get("GITHUB_EVENT_NAME", "push")
    if event_name != "push":
        print(f"Event {event_name!r} is not a push; nothing to do.")
        return 0
    event = _load_event(args.event_path)
    if event.get("deleted"):
        print("Branch deletion has no promotion range; nothing to do.")
        return 0

    ref = event.get("ref")
    if not isinstance(ref, str) or not ref.startswith("refs/heads/"):
        raise ValueError(f"unsupported push ref: {ref!r}")
    target_branch = ref.removeprefix("refs/heads/")
    repository = os.environ.get("GITHUB_REPOSITORY")
    token = os.environ.get("GITHUB_TOKEN")
    if not repository or not token:
        raise ValueError("GITHUB_REPOSITORY and GITHUB_TOKEN are required")

    return process_push(
        client=GitHubClient(
            token,
            api_url=os.environ.get("GITHUB_API_URL", "https://api.github.com"),
        ),
        repository=repository,
        target_branch=target_branch,
        before=str(event.get("before", "")),
        after=str(event.get("after", "")),
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GitHubApiError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
