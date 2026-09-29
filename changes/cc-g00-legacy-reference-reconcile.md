Category: internal
Audience: developers
Breaking-Change: no
Summary: Separate live studio-berry/loop issue records from frozen legacy ones in the preflight catalog overlay (#111) — github_issues entries now carry `repository`, legacy snapshots are keyed `legacy#<n>` and can never be an open row's `closed_by`, the generator adds an opt-in `--verify-github` read-back that rejects a missing issue, a pull request, or a changed title, state, or milestone, sixteen open backlog rows re-point to the reset issues filed for them, and 27 of the 28 legacy issue and pull-request links under docs/ become `legacy #<n>` text (the one in docs/GOVERNED_EXECUTION.md waits for a change that can carry that subsystem's proof lanes), with the convention recorded in docs/LEGACY_ISSUE_PROVENANCE.md.
