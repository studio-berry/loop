# Schema evolution and compatibility

Status: **locked for 0.1.1 session S03**. Product SemVer is independent of these
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
`loupe-preflight/testdata/schemas/`.
