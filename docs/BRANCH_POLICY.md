# Branch policy

The live repository uses two protected lines:

- `dev` is the integration branch.
- `stable` is the release branch and repository default.
- Short-lived topic branches start from `dev`, receive focused changes, and
  merge back to `dev` before release promotion to `stable`.
- `master` is not an active Loupe branch.

The machine-readable policy is [`branch-policy.json`](branch-policy.json).
Keep the policy file and workflow triggers aligned; the generated architecture
catalog records the branch names used by tooling. See issue #232 for the
branch-policy audit and its re-verified repository facts.
