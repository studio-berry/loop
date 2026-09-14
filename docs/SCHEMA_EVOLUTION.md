# Schema evolution and compatibility

Status: **locked**. Product SemVer is independent of these
contracts. See [SEMANTIC_TRUST_ENGINE_PLAN.md](SEMANTIC_TRUST_ENGINE_PLAN.md)
and [schema-compatibility.json](schema-compatibility.json).

## Envelope

Every persisted machine-readable artifact carries:

- `schema_kind` — one of the `PDFSchemaKind` strings
- `schema_version` — integer major (existing writers) or `"MAJOR.MINOR"`

Integer `N` is accepted as major=`N`, minor=`0`. Adding `schema_kind` does **not**
require a report major bump.

## Fail-closed majors

`pdf::checkSchemaCompatibility()` returns `UnsupportedMajor` when the major is
not in `supported_majors`. Callers must not parse that payload as a clean
result. Compatible additive minors preserve unknown fields (`passthrough`).

## Migrations

`migrateSchemaDocument()` is pure and deterministic. History database
migrations already live in `PDFOperationHistoryStore` (v1→v2→v3) and must not
be rewritten here. JSON migrations that change bytes append a provenance
`SchemaMigrated` event; they never rewrite old events.

## Goldens

Current and previous supported versions live under
`loop-preflight/testdata/schemas/`.

## Compatibility diagnostics

`pdf::schemaCompatibilityDiagnostic(kind, version)` is the single diagnostic both
Core and the CLI report. Machine consumers branch on `code`:

| `code` | `PDFSchemaCompatibility` | Meaning |
| --- | --- | --- |
| `schema.compatible` | `Compatible` | The version is supported as written. |
| `schema.unsupported-major` | `UnsupportedMajor` | The major is not in `supported_majors`; the payload must not be parsed. |
| `schema.unknown-kind` | `UnknownKind` | No recognised `schema_kind`. |
| `schema.invalid-version` | `Invalid` | `schema_version` is missing or malformed. |

`PdfTool schema` prints the same `code`, `message`, `schema_kind`, and
`schema_version` values for one artifact, and `PdfTool schema` with no `--input`
prints the compiled matrix, so the CLI and Core expose identical diagnostics.

## Version reporting

`PDFSchemaMigrationResult::fromVersion` is the document's version on entry;
`toVersion` is the version it is at when the call returns. They are equal when
nothing was migrated — a document at a **newer minor** than the matrix current
passes through unchanged, keeps its own version, and keeps unknown fields. Only a
major migration moves `toVersion` to the matrix current.

## Coverage

`pdf::AllSchemaKinds` and `docs/schema-compatibility.json` must describe the same
kinds and versions; `UnitTestsSchemaEvolution` fails when they disagree. Golden
fixtures for every JSON kind live in `loop-preflight/testdata/schemas/`.
`history-db` has no JSON golden: it migrates inside `PDFOperationHistoryStore`,
which records a `SchemaMigrated` provenance event for the upgrade.
