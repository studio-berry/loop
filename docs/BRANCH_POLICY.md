# Loupe branch policy

`stable` is the release line and the repository default branch. `dev` is the
integration line. Short-lived topic branches are created from `dev` and merge
back into `dev`; releases promote reviewed commits from `dev` into `stable`.

The build and CodeQL workflows gate both long-lived branches. Pull requests to
either branch must have the required `ci_ok` status before merging. Direct
pushes and force-pushes are disabled by the corresponding GitHub branch rules.

The declarations below are intentionally machine-readable by
`scripts/ci/check_branch_policy.py`. That check runs in the CI workflow, so a
workflow trigger edited away from this policy fails before the build can be
cited as release evidence.

- CI branches: `dev`, `stable`
- Protected branches: `dev`, `stable`
- Required check: `ci_ok`

`master` is not part of the Loupe branch policy. It is retained only in older
historical documents or upstream references; new workflow triggers must not
target it.
