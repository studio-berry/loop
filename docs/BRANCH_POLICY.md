# Loupe branch policy

`stable` is the release line and the repository default branch. `dev` is the
integration line. Short-lived topic branches are created from `dev` and merge
back into `dev`; releases promote reviewed commits from `dev` into `stable`.

The build and CodeQL workflows run for both long-lived branches. `stable` is
the protected release branch and requires the `release_ok` GitHub Actions
status before merging. That check is produced by the dedicated Release Gate
workflow, which always reports: failed, cancelled, skipped, and missing
dependencies reduce to an explicit terminal failure. `dev` is the integration
branch and intentionally has no required build gate. Direct pushes and
force-pushes are disabled by the corresponding GitHub branch rules.

The Release Gate workflow listens for `pull_request` targeting `stable` and for
`merge_group` so an optional merge queue cannot wait on a check that never
runs. It has no path filters. Integration PRs targeting `dev` run `ci.yml`
without a required aggregate.

The declarations below are intentionally machine-readable by
`scripts/ci/check_branch_policy.py`. That check runs in CI, so a workflow
trigger edited away from this policy fails before the build can be cited as
release evidence. Pass `--live` to also compare these declarations with GitHub
branch protection when a token can read it.

- CI branches: `dev`, `stable`
- Protected branches: `stable`
- Required check: `release_ok`
- Required check app: GitHub Actions
- Release gate workflow: `.github/workflows/release-gate.yml`
- Release gate events: `pull_request`, `merge_group`
- Release gate pull_request branches: `stable`
- Integration workflow: `.github/workflows/ci.yml`
- Integration pull_request branches: `dev`

`master` is not part of the Loupe branch policy. It is retained only in older
historical documents or upstream references; new workflow triggers must not
target it.

After `release_ok` has reported at least once, replace the `stable` required
check `ci_ok` with `release_ok` bound to the GitHub Actions app (id 15368).
Do not require any status check on `dev`.
