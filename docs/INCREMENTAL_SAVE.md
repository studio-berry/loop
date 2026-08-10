# Incremental PDF save

Loupe retains a hash for the opened source and rereads the original bytes when
an edit is safe to preserve in place. The incremental update contains changed
object numbers, a classic xref section, and a trailer whose `/Prev` value points
to the previous xref. The prefix through the previous `%%EOF` is copied
byte-for-byte.

## Save policy

Save mode is declared by the operation through `PDFOperationSavePolicy`; it is
not a global user preference. The policy has three explicit modes:

- `incremental-append` for ordinary annotation/metadata edits where prior
  signed byte ranges and revisions should remain valid;
- `full-rewrite` for redaction, sanitization, destructive cleanup, conversion,
  optimization, and any operation that must remove prior content; and
- `save-as-new-artifact` for production corrections, where the trusted input
  remains immutable and the candidate is written to a separate output artifact.

Each registered `PDFRepairOperation` exposes this policy in its descriptor,
including whether signatures/certificates are invalidated and whether the
operation is reversible in-session. `PDFRepairTransaction::savePolicy()`
combines the policies of all operations conservatively: a new-artifact policy
outranks full rewrite, which outranks incremental append. An unclassified
operation defaults to new-artifact rather than being silently appended.

- An incremental policy may save in place for a signed document or a document
  that already has a `/Prev` chain, preserving the original prefix.
- Save As always uses the existing full-rewrite writer.
- Redaction, sanitization, and other destructive operations must use the full
  writer. The redaction verifier continues to reject a redacted output that
  contains `/Prev`.
- A new-artifact policy cannot overwrite the trusted source; the Editor routes
  it to Save As and rejects an attempt to use the source path as the output.
- If the source cannot be read again, the interactive save is refused rather
  than risking a full rewrite of a signed or revisioned source.
- A changed signature dictionary, removed object slot, source-byte mismatch, or
  encryption-mode change causes incremental save to fail and requires a full
  rewrite or an explicit user-facing recovery path.

The editor content-save path preserves object numbers and does not run the
storage-shrinking optimizer before the controller chooses its write mode. This
is required for changed-object detection and signature coverage preservation.

## Format boundaries

Incremental save currently emits classic xref tables and does not create object
streams or xref streams. Full rewrite retains the existing writer behavior and
therefore remains the format-normalization boundary. Linearization is not
preserved or generated; production export and linearized-input handling remain
outside this save path.

The focused `UnitTestsIncrementalSave` target checks prefix preservation,
changed-object visibility after reopening, source-byte mismatch refusal, and
the write-mode policy. Signed-fixture validation and veraPDF validation remain
external release/CI checks because this repository does not carry a signing
fixture or the veraPDF runtime.
