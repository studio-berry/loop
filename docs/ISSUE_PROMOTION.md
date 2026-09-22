# Issue promotion tracking

`.github/workflows/issue-promotion.yml` keeps GitHub issue state aligned with
the repository's promotion lines:

- A linked issue whose change reaches `dev` is given the existing
  `in promotion queue` label.
- A linked issue that already has that label is closed with GitHub's
  `completed` state reason when the corresponding change reaches `stable`.
  The queue label is then removed.

The workflow listens to `push` on `dev` and `stable`, not to pull-request close
events. That covers topic-branch merges, squash merges, regular merge commits,
fast-forward promotion, and direct commits with one consistent evidence path.
The repository may use `unstable` as an intermediate release-candidate line;
it is intentionally not a closure boundary. Issues remain queued until the
work reaches `stable`.

For each push the workflow asks GitHub for the exact `before...after`
comparison. It also reads merged pull-request titles/bodies and source commits,
which preserves multiple issue links when a squash commit does not retain the
whole PR body.

## Linking convention

Use same-repository references in a commit subject or PR title, such as
`fix: handle bleed (#123)`, or explicit linking language in a PR/commit body,
such as `Closes #123`, `Fixes #124`, `Resolves #125`, `Implements #126`, or
`Related to #127`. Qualified references (`studio-berry/loop#123`) and issue URLs
for this repository are also accepted. References to another repository are
ignored, and pull-request numbers are ignored after GitHub identifies them as
PRs rather than issues.

## Safety rules

- Stable promotion is range-based: only links found in the new push range and
  its associated merged PR/source-commit evidence are considered.
- Stable closure requires the issue to still be open and to already carry
  `in promotion queue`. A direct or partial stable push cannot close an issue
  that never reached `dev` through this state.
- Closed issues are never reopened or relabeled by a back-merge into `dev`.
- API reads complete before any label or close mutation. A truncated,
  non-forward, or failed comparison stops the job without guessing. The
  mutations are idempotent, so rerunning a failed workflow is safe.
- The workflow serializes runs per branch so concurrent pushes do not race
  issue updates. It does not make `issue-promotion` a required branch check;
  the existing `agent-fast / build` and `release_ok` protections remain the
  code/release gates.

If an API outage causes a run to fail, rerun that workflow run. A later push
also compares from its recorded predecessor, so the exact range remains
auditable in the run log.
