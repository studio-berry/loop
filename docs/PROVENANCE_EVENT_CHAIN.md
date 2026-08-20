# Canonical provenance event chain

Issue #237 resolves the overlap between #32 (operation history) and #133
(audit trail/certified-preflight state).

## Canonical choice

There is one runtime event type, `PDFOperationHistoryEvent`, and one append-only
chain, `PDFOperationHistoryStore`. Storage is the existing SQLite history
sidecar/app store (`<pdf>.loupe-history` in the headless paths), never the PDF
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

## Invariants

- Undo/redo is ephemeral editing convenience. Rollback restores a retained
  artifact by appending a forward `RolledBack` event; it never rewrites history.
- Editing or deleting a middle database row breaks sequence/hash verification.
- Invalid certificates/events are retained as history; they are not deleted.
- The chain is tamper-evident, not tamper-proof. A writer who can replace the
  complete sidecar can rewrite the chain. This is attribution and provenance,
  not a digital signature, PKI, or non-repudiation service.
- No parallel `PreflightAuditEvent` type or `.loupe-audit.jsonl` chain is
  permitted.
