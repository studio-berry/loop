# Deterministic preflight profile resolution

Loupe resolves production context into one effective preflight profile before
`PreflightEngine` runs. The engine remains context-blind: it receives the same
validated JSON policy regardless of whether the caller used an explicit file or
contextual selection.

## Context and sources

`pdf::PreflightJobContext` contains stable machine identifiers for client,
product, job type, press, stock, and finishing, plus bounded schema-owned
attributes. IDs are trimmed, Unicode-normalized, and case-folded before
matching. Display names, paths, emails, order notes, and free-form expressions
are not selectors.

`PreflightProfileStore::loadDirectory()` reads a bounded, immutable snapshot of
JSON sources. Files are enumerated by name, malformed files are errors, and
each source records `profile_id`, `profile_version`, priority, selector, and a
canonical SHA-256 content hash. The initial store is local-only; it does not
perform network or database I/O.

Supported selector fields are exact or set-valued `client_id`, `product_id`,
`job_type`, `press_id`, `stock_id`, `finishing_id`, and bounded `attributes`.
Unknown selector fields and arbitrary expressions fail closed.

## Precedence and merging

Matching sources are ordered from broad to specific by:

1. declared `priority`;
2. selector specificity;
3. stable source ID and version for reproducible traversal only.

The final tie-breaker is never used to resolve conflicting policy values. Equal
authority sources with different values fail resolution as ambiguous. Equal
values are accepted.

Scalar and object fields merge recursively. Checks and fixups merge by stable
`id`, never by array position. Omitted fields inherit; an explicit
`enabled: false` is the supported way to disable a check or fixup. `job_types`
is a deterministic set union. Other arrays are replaced as a single semantic
value and therefore participate in the same authority/conflict rules.

The resolver canonicalizes the effective JSON and computes its SHA-256 hash.
The report records the normalized context, matched source IDs/versions/hashes,
the effective hash, resolver version, and field-level override decisions.

## CLI

The existing explicit path remains reproducible:

```text
PdfTool preflight artwork.pdf --profile loupe-default.json
```

Contextual selection uses a local profile store and either direct identifiers
or a structured context document:

```text
PdfTool preflight artwork.pdf \
  --profile-store profiles \
  --client acme \
  --product carton \
  --job-type packaging \
  --press hp-indigo-15k \
  --stock 16pt-sbs \
  --finishing diecut

PdfTool preflight artwork.pdf --profile-store profiles --job-context job.json
```

`--profile` cannot be silently combined with contextual inputs. Explicit mode
is visibly recorded as `profile_resolution.mode = "explicit"`; contextual mode
requires a matching source, and missing stores, malformed sources, missing
required context, and ambiguous matches produce an incomplete failing report.

The Editor sidecar and PageMaster export path use the same Core resolver for
explicit profiles. PageMaster can also provide `PreflightJobContext` and a
profile-store path through `PDFPageMasterExportJob`, allowing one immutable
resolution per export batch.

## Historical and security boundaries

The profile resolution record is part of the normalized preflight report. A
later operation-history or certification layer can bind that record to the
effective profile hash rather than resolving historical jobs against today's
profile files. Profile resolution does not execute fixups, dereference paths,
run PDF inspection, or evaluate scripts.
