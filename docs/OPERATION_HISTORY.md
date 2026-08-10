# Loupe operation history

Loupe durable production provenance is separate from `PDFUndoRedoManager` and `PDFRecoveryManager`.
Undo/redo remains an in-memory interaction feature. Recovery remains disposable crash-recovery state. The
operation history is an append-only Core service for production workflows and automation.

## First-slice contract

`PDFOperationHistoryStore` persists metadata in a SQLite database using WAL mode, foreign keys, bounded busy
timeouts, transactional schema creation, and an explicit schema version. It exposes lifecycle writes only as
new events:

- `planned`, `running`, `accepted`, `rejected`, `failed`, `cancelled`, `interrupted`, and `rolled-back`;
- stable execution and entry UUIDs;
- source/output artifact identities by SHA-256, size, media type, and opaque storage token;
- redacted, canonical JSON parameters and result summaries;
- finding, report, and diff digest references;
- human, policy, and system approval records.

There is no update or delete API for committed history events. `verify()` recomputes the event hash chain and
reports `compromised` rather than rewriting suspicious rows. Local tamper evidence is application-level: a
machine administrator can still replace the entire database, so this is not a substitute for a remotely anchored
audit service or OS-keystore signature.

`PDFArtifactStore` streams SHA-256 hashing in bounded chunks, deduplicates content-addressed artifacts, and
publishes staged files by rename. Logical names are sanitized and the store keeps opaque digest-derived tokens;
raw source paths and document contents do not enter the ledger metadata by default.

Rollback resolution accepts only an artifact referenced by an `accepted` event. The caller must create a new
`rollback` execution and append its lifecycle events; historical rows and accepted artifacts are never rewritten.

The shared Core contract is intentionally delivered before Editor and PdfTool adapters. Those surfaces must use
this API rather than introducing separate history stores. Action List parents and corrective-operation plans can
use `parentExecutionId`, canonical parameters, and digest references without coupling Core repair primitives to
GUI or database details.
