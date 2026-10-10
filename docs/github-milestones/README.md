# Retired release milestones

The version-named Markdown files in this directory preserve pre-reset planning.
Their issue numbers, milestone numbers, completion states, and proposed versions
belong to the retired repository. They do not describe current Loop acceptance.

[The approved master roadmap](https://app.notion.com/p/3bb9cb079ddb80c4a15feaa98f963f4c)
replaced release buckets with L01-L14 capability milestones and R00, G00, and X00.
GitHub is the current milestone inventory.

The [G00 decision](../REPOSITORY_GOVERNANCE.md) retires this directory as an input
to remote writes. The dispatch workflow and active manifest have been removed.
The compatibility command `python scripts/github/sync_milestones.py` reports
retirement and plans no changes. `--apply` exits with code 2 before contacting
GitHub. Historical descriptions stay available for source reconciliation.
