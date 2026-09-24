# Issue promotion tracking

`.github/workflows/issue-promotion.yml` records the highest promotion branch
known to contain linked issue work. It updates the organization-wide
`Promotion stage` single-select issue field (ID `47367010`):

| Branch reached | Field value |
| --- | --- |
| `dev` | `Dev present` |
| `unstable` | `Unstable present` |
| `stable` | `Stable present` |

Branch presence is not issue acceptance. The workflow never changes an issue's
open/closed state. A module gate or sub-issue closes only after its own acceptance
evidence is reviewed. The legacy `in promotion queue` label and its automatic
stable-branch closure rule are retired in loop2.

The workflow listens to pushes on all three branches. For each push it compares
the exact `before...after` range, then reads merged PR titles, bodies, and source
commits. This preserves issue links across squash promotion. It updates a field
only if the new branch is later in the promotion chain than the current value;
back-merges and delayed runs cannot intentionally downgrade it. An issue can
move directly to `Stable present` if the stable push provides the first usable
link. The field records observed branch membership, not a required path.

## Linking convention

Use same-repository references in a commit subject or PR title, such as
`fix: handle bleed (#123)`, or explicit linking language in a PR or commit body,
such as `Closes #123`, `Fixes #124`, `Resolves #125`, `Implements #126`, or
`Related to #127`. Qualified references (`studio-berry/loop2#123`) and issue
URLs for this repository are also accepted. References to other repositories
are ignored, and PR numbers are ignored after GitHub identifies them as PRs.

## Safety and recovery

- Every issue and its current field value is read before any field is changed.
  Truncated, non-forward, or failed comparisons stop the job without guessing.
- Existing field values are preserved by the additive issue-field API call.
  Unknown promotion options fail explicitly rather than being overwritten.
- Runs are serialized across promotion branches. Re-running a failed workflow
  is safe because equal or later field values are left alone.
- The workflow does not create or remove labels and does not close or reopen
  issues. GitHub's separate linked-PR auto-close setting still applies to PRs
  merged into the default branch; avoid closing keywords on acceptance-gated
  issues unless that behavior is intended.
- If an API outage causes a run to fail, rerun that workflow run. A later push
  compares from its own recorded predecessor, so the original range remains
  auditable in the failed run.

`Promotion stage` is an organization issue field, so changing or recreating it
requires updating `PROMOTION_FIELD_ID` in `scripts/github/issue_promotion.py`.
The workflow's `issues: write` permission is needed to set values. It is not a
release check; the existing code and release gates remain authoritative.
