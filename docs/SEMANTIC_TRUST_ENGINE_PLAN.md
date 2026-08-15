# Semantic Trust Engine — Implementation Plan

Status: **locked for 0.1.1** (M0). Former planning alias: **0.0.3**. Living product gate: **0.1.1**.
Scope: UI-neutral Core / PdfTool / tests. Product `PDF4QT_VERSION` stays **0.1.0** until session S18’s exit checklist is green.
Related: GitHub #234, #236, #237, #238, #239, #240, #241, #242, #243, #244, #245, #246, #266, #267, #268, #269, #125, #128, #129, #132, #165.

## Goal

Establish one revision-safe, evidence-driven contract:

```text
artifact + revision
  -> bounded inspection
  -> normalized evidence
  -> policy evaluation
  -> verdict
  -> operation impact
  -> revalidation
  -> provenance
```

The milestone is complete only when incomplete inspection, unsupported fidelity, cancellation, schema incompatibility, and hostile workload limits cannot silently become a clean PASS.

## Non-goals

- Qt Quick shell, canvas host, finding-navigation UI, visual inspector UI
- Production correction UX, AI execution
- Product SemVer bump before S18
- Coupling a report schema bump to a product patch
- Bundling veraPDF / a JRE
- A second audit ledger or parallel rule engine beside the Evidence Graph

## Surface order

1. Core types and `apply()` / `evaluate()` APIs
2. PdfTool as the non-interactive harness
3. Unit tests and golden fixtures
4. Editor / PageMaster consume the same objects with **no new GUI**

## Identity lock

Do **not** invent a second freshness type.

| Spec name | Code type | Role |
|-----------|-----------|------|
| persisted artifact identity | `PDFArtifactIdentity` (`pdfartifactidentity.h`) | Content-addressed bytes: sha256, size, mediaType, logicalName, storageToken |
| in-session document | `PDFDocumentIdentity` | Pointer-derived `documentId` + `sourceDataHash` |
| revision token | `PDFRevisionToken` | Alias of `PDFRevisionIdentity`, not a distinct struct |

```cpp
using PDFRevisionToken = PDFRevisionIdentity;
```

`PDFRevisionIdentity` / `PDFRevisionToken` = `PDFDocumentIdentity` + `documentRevision` + `cacheGeneration` + `effectiveProfileIdentity`.

Rules:

- Every cache entry, job, and asynchronous result carries `PDFRevisionIdentity`.
- Persisted provenance events and artifact-store records use `PDFArtifactIdentity`.
- A mismatch with `PDFDocumentContext::getRevision()` is discarded; it is never reconciled heuristically.

## Frozen Core type names

| Type | Header | Purpose |
|------|--------|---------|
| `PDFSchemaKind` | `pdfschemaversion.h` | Kind of a persisted machine-readable artifact |
| `PDFSchemaVersion` | `pdfschemaversion.h` | `{ quint16 major, minor }` |
| `PDFSchemaCompatibility` | `pdfschemaversion.h` | Compatible / UnsupportedMajor / UnknownKind |
| `PDFEvidenceDomain` | `pdfevidencegraph.h` | Images, Colorants, Strokes, OverprintTransparency, Fonts |
| `PDFEvidenceRecord` | `pdfevidencegraph.h` | One normalized observation |
| `PDFEvidenceGraph` | `pdfevidencegraph.h` | Revision-bound bag of records |
| `PDFOperationImpact` | `pdfoperationimpact.h` | Declared revalidation impact |
| `PDFPluginInfo::abiVersion` | `pdfplugin.h` | Integer ABI inspected before `QPluginLoader::instance()` |

JSON field names (locked):

| Field | Meaning |
|-------|---------|
| `schema_kind` | Additive string kind (`preflight-report`, …) |
| `schema_version` | Integer major (existing) **or** `"MAJOR.MINOR"` string for new writers |
| `evidence_ids` | Array of evidence record ids cited by a finding |
| `impact_complete` | Whether `PDFOperationImpact` is a complete description |

Integer `schema_version: N` already on disk is accepted as major=`N`, minor=`0`. Do **not** bump report major merely to add `schema_kind`.

## PDFSchemaKind values

`preflight-report`, `preflight-profile`, `evidence-graph`, `operation-plan`, `operation-result`, `provenance-event`, `certificate`, `capability-discovery`, `package-manifest`, `action-list`, `pdftool-envelope`, `ocr-report`, `history-db`, `pagemaster-manifest`, `preflight-decisions`.

`evidence-graph` is reserved in the compatibility matrix from S03; populated in S06.

## PDFEvidenceRecord fields

- `id` (stable)
- `producer`, `producerVersion`
- `artifact` (`PDFArtifactIdentity`)
- `revision` (`PDFRevisionIdentity`)
- `domain` (`PDFEvidenceDomain`)
- `page`, `objectId`, semantic `target`
- `observedValue`, `units`
- optional `geometry` (`QRectF`)
- `coverageMethod` / sampling
- `fidelity` / `confidence`
- `incompleteReason`
- `budgetContext`

An incomplete graph cannot reduce to PASS.

## PDFOperationImpact fields

- `domains` (`PDFEvidenceDomain` flags)
- page/object scope
- `documentWide`
- `fullRewrite`
- `impactComplete`

Unknown or incomplete impact → full revalidation. Cache invalidation and check selection share this model.

## Fail-closed table

| Condition | Terminal state | Must not become |
|-----------|----------------|-----------------|
| Inspection incomplete | `incomplete` | PASS |
| Budget exceeded, zero findings | `incomplete` | PASS |
| Engine / profile / document failure | `error` | PASS |
| Unsupported schema major | parse/load **error** (or report `error`) | clean parse |
| Cancellation | job `Cancelled`; no success kind on provenance | `Succeeded` / PASS |
| Unresolved profile variable or empty/unsupported scope | `incomplete` | PASS |
| Independent oracle missing or mismatch | conversion **error**; no certificate | self-certified PASS |
| Recovered / unapproved output | not certified | `CertificateIssued` as current |

`reducePreflightVerdict()` remains the only operator-facing reducer. The legacy `pass` boolean is derived.

## Schema vs product SemVer

Product versions follow [VERSIONING.md](VERSIONING.md). JSON/`history-db` schema versions are a **separate** contract ([SCHEMA_EVOLUTION.md](SCHEMA_EVOLUTION.md) from S03). Never bump `PDF4QT_VERSION` because a schema minor moved, and never bump a report major because a product patch shipped.

Compatibility:

- Unsupported **major** → fail closed
- Compatible additive **minor** → preserve unknown fields where the matrix says `passthrough`
- Migrations are pure, deterministic, tested, and provenance-visible when they rewrite bytes (`SchemaMigrated` event; never rewrite old events)

## Agent sessions (0.1.1)

| ID | Goal |
|----|------|
| S00 | This document |
| S01 | `PDFRevisionToken` alias + fail-closed acceptance tests (#234/#236/#239) |
| S02 | Live `PreflightRun` / `FixApplied` provenance on PdfTool (#237) |
| S03 | Schema Core API + matrix + goldens + `migrate()` (#266) |
| S04 | PageMaster revision fence + thumbnail scheduler (#236/#238) |
| S05 | Remaining long-lived jobs; cancel ≠ success |
| S06 | Evidence Graph types, collector, dual-run, images family (#240) |
| S07 | Other four families + golden-corpus parity |
| S08 | Switch checks onto the graph; delete old walkers |
| S09 | Profile identity, restrictions, variables (#132/#125/#128) |
| S10 | Generated check catalog + coverage matrix (#165) |
| S11 | `PDFOperationImpact` + targeted = full revalidation (#267) |
| S12 | Independent oracle lane + conversion fixture triad (#241) |
| S13 | Renderer differentials for color/overprint goldens |
| S14 | Named budget pools + synthetic exhaustion corpus (#242/#243) |
| S15 | Huge-document envelope + quality/prefetch shedding |
| S16 | Model-based lifecycle sequences (#268) |
| S17 | Plugin ABI trust + upstream divergence register (#269/#246) |
| S18 | Benchmarks + architecture invariants; exit audit; version bump only if green |

Parallel after S00: S01, S02, S03, S17. Never start S08 until S07 parity is green.

## Already landed

- #234 `reducePreflightVerdict()` in `pdfpreflightverdict.h`, `docs/PREFLIGHT_VERDICT.md`
- #236 `PDFDocumentContext` in `pdfdocumentcontext.h`, `docs/REVISION_CONTEXT.md`
- #237 one SQLite chain, schema v3, `pdfoperationhistorystore.*`
- #238 `PDFJobScheduler` Core API
- #239 `PDFOperationSavePolicy` on repair operations

## Catalog

After adding schema kinds, checks, operations, or test targets, regenerate:

```text
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

Do not hand-edit `docs/generated/architecture-catalog.json`.

## Open work

Product `PDF4QT_VERSION` stays **0.1.0**.

- **S08.** Image-resolution still walks annotation appearance streams the collector does not visit. Checks still use per-check processors; the graph is collected and cited. Start S08 only after the collector covers those appearances without changing corpus findings.
- **S18.** S08 walkers remain; veraPDF is skip-if-missing. Do not bump the product version yet.
- **S05 leftovers.** Page compile, PageMaster export, and Editor preflight are not on `PDFJobScheduler`.
