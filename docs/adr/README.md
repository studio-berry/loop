# Architecture decision records

**Status:** accepted
**Implemented-at:** e65cdd19e0fb876adf367f0387c63996efcfebfa
**Last-verified:** 2026-09-13 @ e65cdd19e0fb876adf367f0387c63996efcfebfa
**Superseded-by:** none

Loop records durable product and engineering decisions as ADRs under this
directory. Each numbered ADR file carries machine-checked metadata:

```
Status: proposed | accepted | implemented | superseded
Implemented-at: <commit sha or milestone note>
Last-verified: <YYYY-MM-DD> @ <commit sha>
Superseded-by: <adr-id or none>
```

CI validates every header through
[`scripts/generate-architecture-catalogs.py`](../../scripts/generate-architecture-catalogs.py)
in the `Documentation truth` workflow (see [`CI.md`](../CI.md)).

## Resolving conflicts

When narrative docs disagree with code, follow the hierarchy in
[`architecture-source-of-truth.md`](../architecture-source-of-truth.md).
Revision fencing and cache authority are specified in
[`REVISION_CONTEXT.md`](../REVISION_CONTEXT.md).

## Generated factual inventories

Do not hand-maintain parallel lists of checks, operations, schema versions, test
targets, or branch names. Regenerate and commit:

| Artifact | Contents |
|----------|----------|
| [`generated/architecture-catalog.json`](../generated/architecture-catalog.json) | Branch policy, workflow branches, preflight check IDs, repair operations, schema versions/kinds, architecture invariants, CMake test targets |
| [`generated/preflight-check-catalog.json`](../generated/preflight-check-catalog.json) | Per-check measures, limitations, and GWG/PDF-X coverage overlay |

```bash
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

## Index

| ADR | Status | Topic |
|-----|--------|-------|
| [001](adr-001-pdf-document-session.md) | implemented | `PDFDocumentSession` lifetime, revision-keyed caches, thread-safety |
| [002](adr-002-preflight-engine-orchestrator.md) | implemented | `PreflightEngine` orchestrator boundary and check registry |
| [003](adr-003-pagemaster-export-orchestrator.md) | implemented | PageMaster export stage order |
| [004](adr-004-pagemaster-batch-manifest.md) | implemented | PageMaster batch manifest and checkpoints |
| [005](adr-005-product-surface-pruning-classification.md) | accepted | Product surface pruning classification |
| [006](adr-006-rotating-logs-and-diagnostics-bundle.md) | implemented | Rotating logs and diagnostics bundle |
| [007](adr-007-qt-quick-controls-shell.md) | accepted | Qt Quick Controls shell foundation |
| [008 (semver)](adr-008-semantic-versioning.md) | accepted | Semantic versioning policy |
| [008 (history)](adr-008-generated-history-rewrite.md) | implemented | Generated-artifact history rewrite |
| [009](adr-009-canvas-hosting-benchmark.md) | accepted | Canvas hosting benchmark admission |
| [010](adr-010-quick-root-admission.md) | accepted | Quick root admission contracts |

Implementation tracking for ADR-005 lives separately in
[`ADR-005-product-surface-pruning-implementation-plan.md`](../ADR-005-product-surface-pruning-implementation-plan.md).
