# Align the product version to 0.2.0-alpha

Category: internal
Audience: developers and release operators
Breaking-Change: no
Summary: Set LOUPE_VERSION to 0.2.0 with LOUPE_VERSION_PRERELEASE alpha and align
docs/version-policy.json, docs/VERSIONING.md, and the generated policy adapters. The 0.2.0
orchestration line was already the working milestone label but every branch still read
0.1.0, so the repository disagreed with the plan of record. This is a label alignment only:
no build, packaging, schema, or CLI behavior changes, and the 0.1.x milestone remap table is
retained as history.
