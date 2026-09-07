# Session 11 — Prove production resource envelope

## Scope

Session 11 closes issue #242 measurement gaps for gate **R-01** on the merged
`dev` candidate SHA recorded at PR merge time, anchored to `origin/dev` @
`6a55130c…`.

Fixtures remain outside the repository per #242.

## Issue 34 — Fixture manifest

Partial manifest with digests for office-2mb, pathological-vector, transparency-spots.
Requirement catalog documents all issue #242 IDs. External bytes under
`C:\.dev\qualification\session-11\fixtures\`.

## Issue 35 — Measure envelopes

Local Windows `run_matrix.py --strict`: exit 1, `measured: 0`, fail-closed on stale
PdfTool commit and missing fixtures. `-1` fields never promoted to pass.

## Issue 36 — Archive

Frozen `docs/evidence/session-11-resource-envelope/evidence.json` with
`disposition: incomplete`. CI validates via
`scripts/qualification/validate_resource_envelope_evidence.py`.

## Exit gate

R-01 remains **Partial** until hosted Linux + full fixture bundle on candidate-SHA PdfTool.
