# Preflight check catalog, GWG / PDF/X coverage, and the coverage backlog

Loop publishes a generated check catalog, a coverage matrix, a prioritised
coverage backlog, and the corpus-coverage map behind its `covered` rows, so a
clean preflight pass is read as **clean against this scope**, not as a Ghent
Workgroup certificate.

## Sources

- Registry: `PreflightEngine::registerBuiltInChecks()`
- Overlay: [`docs/preflight-check-catalog-overlay.json`](preflight-check-catalog-overlay.json)
- Generated catalog: [`docs/generated/preflight-check-catalog.json`](generated/preflight-check-catalog.json)
- Generated backlog: [`docs/generated/preflight-coverage-backlog.json`](generated/preflight-coverage-backlog.json)
- Generated corpus coverage: [`docs/generated/preflight-corpus-coverage.json`](generated/preflight-corpus-coverage.json)

Regenerate after adding or renaming a check, or after triaging a coverage gap:

```text
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

`--check` fails when a registered check has no overlay entry, an overlay entry
names a check that is not registered, a row is missing a required field, a
backlog row is malformed, or a corpus claim has no fixture behind it.
`scripts/ci/test_preflight_check_catalog.py` and
`scripts/ci/test_preflight_corpus_coverage.py` pin each of those failures.

## Check rows

Every row is keyed by the check id and carries:

| Field | Meaning |
|-------|---------|
| `measures` | What the check inspects. |
| `limitations` | What it knowingly does not do. |
| `coverage` | `covered`, `partial`, or `not_covered` for this row. |
| `families` | The GWG process families the check serves. |
| `parameters` | Every profile-side value the registered implementation reads, as `{id, type, default, range, meaning}`. |
| `severity` | The finding types the check emits as `{finding_type, severity, condition}`, mirroring how the engine sets `PreflightFinding::severity`. |
| `evidence` | The report fields the check's findings carry. |
| `fixups` | Registered preflight fixup ids that remediate this check's findings; `[]` when none does. |

Sourcing rules, so the rows cannot drift into prose:

- `parameters` ids are profile check fields the engine's own parser reads
  (`PreflightEngine::parseProfile`), not merely fields the published profile
  schema lists. Defaults come from the `PreflightCheckConfig` member
  initialisers and the parser's per-check fallbacks.
- The shared check envelope (`id`, `severity`, `enabled`) is excluded from
  `parameters`; `severity` is described by the `severity` block instead.
- `evidence` names use the report field names emitted by `findingToJson`. A key
  of a check's own `evidence` object is written `evidence.<key>`. The
  engine-applied `scope_restrictions` field is excluded because it is not set at
  a check's finding construction site.
- A `fixups` id must be a repair operation whose `isPreflightFixup()` is true.
  Every registered preflight fixup must be claimed by at least one row, and a
  fixup is only listed on a check whose findings the engine (or, for
  `downsample-images`, the audited correction-operation catalog) ties it to.

## Coverage values

Each check is `covered`, `partial` (limitation named in the overlay), or
`not_covered`. Process families follow GWG's sheetfed offset, web offset,
packaging, newspaper, and digital printing groups, plus Loop's audited PDF/X
targets. See also [`PDFX_POLICY_MATRIX.md`](PDFX_POLICY_MATRIX.md).

## Corpus coverage

The golden corpus is the only evidence behind a `covered` row, so the map between
the two is generated and published rather than assumed:

> A matrix row is exercised by a corpus fixture when that fixture's committed
> report snapshot carries at least one finding in `errors[]` or `warnings[]`
> whose `check_id` is the row and whose type is not an inspection-failure type
> (`budget-exceeded`, `check-error`, `check-incomplete`, `evidence-incomplete`).
> A check that is merely enabled, that reports incomplete, skipped, non-inspected
> or non-applicable inspection, or that the fixture's profile disables does not
> count as exercising its row; those fixtures are listed in
> `uninspected_fixtures`. A finding that reports an inspection failure is not
> evidence that the check detects the defect.

A finding is the check reporting on the document. An inspection failure is the
check reporting on itself, and it must not read as coverage. `thin-parts-fill`
exists to exercise `thin-parts`, and it reports `check-incomplete` because its
0.24 pt measurement sits inside the 0.12 pt raster pixel (at `probe_dpi` 600) of
the 0.25 pt threshold, so `thin-parts` is recorded as a corpus gap rather than as
covered.

Regenerate after adding or removing a fixture, or after a check's findings change
(which is also when the snapshot is rewritten):

```text
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

`--check` fails when a `covered` row has no exercising fixture, when a row with
no exercising fixture carries no `corpus_gap` reason, when a row carries a
malformed or a stale reason, when a fixture has no committed snapshot, when a
snapshot has no fixture, when a snapshot names an unregistered check, or when the
engine's own coverage scope stamps a different `matrix_id` than the overlay
publishes. Two neighbouring rules fail earlier in the same run: the catalog
builder rejects a registered check whose overlay row is missing (`registered
without catalog: <id>`), and the corpus loader rejects an empty manifest, an entry
with no `id`, or a duplicate fixture id.

`corpus_gaps` in the generated map is the register of rows the corpus does not
exercise at all. A row is a limitation of the evidence, not necessarily of the
check. A check can be registered, enabled and correct while no fixture has ever
produced one of its findings, and that is exactly what must not read as proven.
Each row carries the two evidence sets and one count: `finding_fixtures`, the
fixtures where the check reported on the document, `uninspected_fixtures`, the
fixtures where it did not report on the document at all (incomplete, skipped,
non-inspected or not-applicable inspection, scope exclusions included), and
`clean_fixture_count`, how many fixtures it inspected with status `ok` and no
attributed findings.

## Coverage backlog

`backlog` in the overlay is the prioritised register of defect classes the
matrix does not close, emitted to the generated backlog file. Each row carries
exactly `id`, `priority`, `gap`, `families`, `state`, `closed_by`, and
`deferral`.

The priority rule is stated once in the generated file (`priority_rule`):

- **P1** — no registered check inspects the defect class at all (a matrix
  `not_covered` entry), so a clean run is silent about it.
- **P2** — a registered check inspects the class, but its named limitation can
  suppress or misclassify a finding on defective content, which would be a false
  clean pass.
- **P3** — a registered check inspects the class and the named limitation only
  narrows reported detail or fails closed as incomplete, so it cannot turn a
  defect into a silent pass.

State and closure (`state_rule` in the generated file):

- `open` — the gap is still present.
- `landed` — a registered check now covers the class; `closed_by` names that
  check id.
- `closed` — the filed issue that tracked the gap is closed.

`closed_by` is a verified GitHub issue (`#<number>`), a registered check id, or
the literal `unfiled` when neither exists. Issue numbers are never inferred: the
overlay records each one in `github_issues` with the number, title, state, and
milestone read back from `gh issue view`, the generator refuses a reference with
no verified record, and it refuses a row whose `state` disagrees with the
recorded issue state — so a closed issue forces a row to be re-triaged rather
than left stale. Every `not_covered` class must appear in a P1 row's `gap`, and
no P1 row may invent a class the matrix does not list.

A row that is not filed carries a `deferral` reason instead, and the generator
refuses both an unfiled row without one and a filed row that still carries one —
so "not filed yet" is always a reviewed statement rather than an omission.

## Claim

**Loop does not claim formal GWG conformance.** In particular, no GWG 2022 or
2024 certificate requirements are evaluated for `sheetfed-offset` or
`packaging`. A clean result for either family reports only on the checks enabled
in that run; it says nothing about the family's GWG certificate. The report's
`coverage_scope.claim` states this limit alongside the enabled check ids for
that run.

Uncovered classes currently include GWG 2022/2024 certificates, PDF/VT
variable-data streams, per-named-colorant ink limits beyond inventory,
barcode/slug/Braille validation, and imposition. Declared PDF/X-5n, PDF/X-5g,
and PDF/A-3 claims are reported unsupported by `conformance-claims` rather
than left as a silent gap.
