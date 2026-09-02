# Align the product version to 0.2.0-alpha

Category: internal
Audience: developers and release operators
Breaking-Change: no
Summary: Set LOOP_VERSION to 0.2.0 with LOOP_VERSION_PRERELEASE alpha and align
docs/version-policy.json, docs/VERSIONING.md, generated policy adapters, ADR-008, RELEASES.txt,
plugin BuildIds, and the preflight report example. The 0.2.0 orchestration line was already the
working milestone label but every branch still read 0.1.0, so the repository disagreed with the
plan of record. This is a label alignment only: no build, packaging, schema, or CLI behavior
changes, and the 0.1.x milestone remap table is retained as history. Also repair preflight plugin
test fixtures so schema v2 scope examples include evidence_ids required after v2→v3 migration.
