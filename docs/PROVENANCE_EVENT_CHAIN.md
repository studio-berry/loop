# Canonical provenance event chain

Issue #237 resolves the overlap between #32 (operation history) and #133
(audit trail/certified-preflight state).

## Canonical choice

There is one runtime event type, `PDFOperationHistoryEvent`, and one append-only
chain, `PDFOperationHistoryStore`. Storage is the existing SQLite history
sidecar/app store (`<pdf>.loop-history` in the headless paths), never the PDF
bytes. The PDF is therefore not changed merely by recording provenance.

`PDFOperationHistoryEventKind` carries the #133 vocabulary:

| #133 event | Canonical kind |
| --- | --- |
| `DocumentOpened` | `DocumentOpened` |
| `PreflightRun` | `PreflightRun` |
| `FixApplied` | `FixApplied` |
| `DecisionRecorded` | `DecisionRecorded` |
| `DecisionInvalidated` | `DecisionInvalidated` |
| `CertificateIssued` | `CertificateIssued` |
| `CertificateInvalidated` | `CertificateInvalidated` |

The shared event fields are `kind`, `operatorIdentity`,
`documentRevisionDigest`, `effectiveProfileDigest`, report/diff digest,
`approval.decisionReference`, and the existing artifact/output identities.

PdfTool `preflight` appends live `PreflightRun` events (with revision and
profile digests) on the document sidecar. PdfTool `repair` appends `FixApplied`.
Cancellation and write failure append `Cancelled` / `Failed` of that kind —
never a success/`Accepted` kind. The default `Operation` kind is not used for
those live commands.
The canonical naming maps #133's `eventId` to `entryId`, `eventDigest` to
`eventHash`, and `previousEventId` to the prior event's `previousEventHash` /
current-chain predecessor. `previousEventHash` and `eventHash` remain the only chain links. Schema-v2
operation hashes remain verifiable during the version-3 migration; provenance
kinds hash the new fields as part of their canonical payload.

## Acceptance guard

`scripts/ci/check_source_integrity.py` enforces the runtime convergence rule on
tracked production sources. It rejects reintroduction of the superseded
`PreflightAuditEvent` / `PreflightAuditStore` declarations and legacy audit
JSONL sidecar literals. Documentation
and interchange schemas may describe provenance, but runtime persistence must
continue to terminate in `PDFOperationHistoryStore`.

The operation-history unit suite separately proves that editing or deleting a
middle record compromises verification and that rollback appends forward
without removing prior events.

## Portable export of the chain

`PdfTool export-evidence-bundle` copies the complete chain for a revision into a bundle member
([`PREFLIGHT_EVIDENCE_BUNDLE.md`](PREFLIGHT_EVIDENCE_BUNDLE.md)). The chain hashes the report payload
as persisted, and that payload names the source path, so the exported slice cannot carry the
canonical per-event hashes: the exporter redacts paths first and recomputes `previousEventHash` /
`eventHash` over the redacted copy. The manifest records `chain_mode: "path-redacted"`, the head
event id and hash, and `canonical_chain_digest` — a digest over the canonical
`(sequence, entryId, eventHash)` triples — so an auditor holding the sidecar can bind the exported
slice to it. Offline verification proves the exported chain is internally consistent and unchanged
since export; it does not re-derive the canonical hashes, and the bundle is not a second ledger.

## Invariants

- Undo/redo is ephemeral editing convenience. Rollback restores a retained
  artifact by appending a forward `RolledBack` event; it never rewrites history.
- Editing or deleting a middle database row breaks sequence/hash verification.
- Invalid certificates/events are retained as history; they are not deleted.
- The chain is tamper-evident, not tamper-proof. A writer who can replace the
  complete sidecar can rewrite the chain. This is attribution and provenance,
  not a digital signature, PKI, or non-repudiation service.
- No parallel `PreflightAuditEvent` type or `.loop-audit.jsonl` chain is
  permitted.
