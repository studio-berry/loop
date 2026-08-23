Category: fixed
Audience: developers
Breaking-Change: no
Summary: Fix UnitTestsPreflightPlugin gating evidence_ids on the post-migration schema version instead of the version a report was submitted as (wrongly rejecting legacy v2 findings), and backfill schema_kind onto 5 corpus snapshots that predated PreflightResult::toJson() emitting it, fixing UnitTestsPreflightCorpus.
