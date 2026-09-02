# Loop operation history

Loop durable production provenance is separate from `PDFUndoRedoManager` and `PDFRecoveryManager`.
Undo/redo remains an in-memory interaction feature. Recovery remains disposable crash-recovery state. The
operation history is an append-only Core service for production workflows and automation.

This is the runtime chain for the certified audit trail described by issue #133: the operation event hash chain
is the audit ledger, and rollback points reference those event ids rather than maintaining a parallel history.

Issue #237 makes this convergence explicit: #32's `PDFOperationHistoryEvent` is the one canonical event type and
`PDFOperationHistoryStore` is the one SQLite-backed chain. #133 event kinds are represented by
`PDFOperationHistoryEventKind` values (`DocumentOpened`, `PreflightRun`, `FixApplied`, `DecisionRecorded`,
`DecisionInvalidated`, `CertificateIssued`, and `CertificateInvalidated`). There is no `PreflightAuditEvent`,
`.loop-audit.jsonl`, or second hash chain in Core.

## Runtime contract

`PDFOperationHistoryStore` persists metadata in a SQLite database using WAL mode, foreign keys, bounded busy
timeouts, transactional schema creation, and an explicit schema version. It exposes lifecycle writes only as
new events:

- `planned`, `running`, `accepted`, `rejected`, `failed`, `cancelled`, `interrupted`, and `rolled-back`;
- stable execution and entry UUIDs;
- source/output artifact identities by SHA-256, size, media type, and opaque storage token;
- redacted, canonical JSON parameters and result summaries;
- finding, report, and diff digest references;
- human, policy, and system approval records.
- event kind, operator identity, document revision digest, effective profile digest, and decision reference for
  provenance events.

There is no update or delete API for committed history events. `verify()` recomputes the event hash chain and
reports `compromised` rather than rewriting suspicious rows. Local tamper evidence is application-level: a
machine administrator can still replace the entire database, so this is not a substitute for a remotely anchored
audit service or OS-keystore signature.

`PDFArtifactStore` streams SHA-256 hashing in bounded chunks, deduplicates content-addressed artifacts, and
publishes staged files by rename as read-only files. Logical names are sanitized and the store keeps opaque
digest-derived tokens; raw source paths and document contents do not enter the ledger metadata by default.

Every accepted or rolled-back event creates one `PDFRollbackPoint` in the sidecar ledger. A point records the
rollback id, producing audit event id, document revision digest, UTC timestamp, content-addressed artifact path,
byte count, operation id, plan summary, approval protection, and visible eviction state. The original input is
registered as a protected point before the first operation. The artifact file is never embedded in the PDF.

`PDFHistoryRetentionPolicy` defaults to 20 points, 2 GiB, 90 days, and protection for the original input and
approved outputs. Eviction removes only the digest-addressed artifact; the immutable event and rollback point
remain and are marked `artifact_evicted`. Shared artifacts are retained until their last point is evicted.

`rollbackTo()` accepts only a non-evicted artifact referenced by an accepted event. It verifies the digest before
opening the destination `QSaveFile`, then appends a new `history.rollback` execution and `rolled-back` event.
Intervening history is never erased. A corrupt or missing target fails before the current document is touched.

The headless `PdfTool repair` and `action-list run/batch` paths create a per-output `<pdf>.loop-history` sidecar
using this API. Editor and PageMaster UI wiring remains deferred until after 0.1.1; those surfaces must use this
same Core chain when enabled rather than introducing separate history stores. Action List parents and corrective-
operation plans can use `parentExecutionId`, canonical parameters, and digest references without coupling Core
repair primitives to GUI or database details.

The database schema migrates the earlier version-1 history foundation to version 2 by adding rollback-point and
artifact eviction state, then version 3 adds the canonical provenance fields to the same chain. The sidecar remains
separate from the PDF and can be copied, inspected, and retained as an operational record. A tamper-evident chain
is not tamper-proof: anyone with write access to the complete database can rewrite it, so this is not a digital
signature or a PKI trust assertion.
