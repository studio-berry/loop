# Incremental PDF save

Loop retains a hash for the opened source and rereads the original bytes when
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
- A new-artifact policy cannot overwrite the trusted source: Core refuses a candidate
  write whose output resolves to the transaction's `sourcePath`, and PdfTool refuses
  the same path for the corrective commands it drives.
- If the source cannot be read again, the interactive save is refused rather
  than risking a full rewrite of a signed or revisioned source.
- A changed signature dictionary, removed object slot, source-byte mismatch, or
  encryption-mode change causes incremental save to fail and requires a full
  rewrite or an explicit user-facing recovery path.

### Policy strength

The declared policy is a floor, not a hint. `savePolicyIsWeaker()` compares a
caller's request with the operation's declaration: a weaker mode, an unstated
signature loss, or a claimed reversibility the operation does not have. A
strictly stronger mode is never weaker, so asking for more safety is always
allowed. `PDFRepairTransaction::setRequestedSavePolicy()` accepts a stricter
request and refuses a weaker one before `analyze()`, `apply()` or
`serializeCandidate()` do any work, and the refusal is remembered so a caller
cannot retry past it. `pdf::validateSaveRequest()` applies the same rule at the
save boundary and additionally refuses a candidate whose output resolves to
`PDFRepairTransactionOptions::sourcePath`. PdfTool reports the refusal as
diagnostic `save-policy.refused` with exit code 4.

`PdfTool repair` validates its publish destination with the same helper before
`publishGovernedArtifact`; `--output <source>` is refused with diagnostic
`save-policy.refused` and exit code 4 even when `--overwrite` is passed, because
repair never appends in place.

The Action List carries the same contract. Every step plan reports the
operation-declared policy as `save_policy`, and
`PDFActionListExecutor::mergedSavePolicy()` merges the policies of all step
operations conservatively onto the execution result. The merged policy is part
of the Action List plan digest, so a policy change moves the digest and
invalidates approvals bound to it. `PDFActionListExecutionOptions::requestedSavePolicy`
may ask for more safety than the declaration, but a weaker request is refused
with diagnostic `action-list.save-policy-refused` before any step work (dry
runs included). `PdfTool action-list run`/`batch` and the Editor publication
boundary validate the save request before writing the candidate.

These guarantees are pinned by name, not by convention:
`everyRegisteredOperationDeclaresItsSavePolicy` (no registered operation may
rely on the undeclared default),
`transactionRejectsAWeakenedSavePolicyBeforeMutation`,
`saveRequestRefusesToWriteOverTheTrustedSource`,
`candidateSaveRefusesToOverwriteTheSourceOnDisk`,
`sourceBytesSurviveSuccessCancelAndFailure`,
`noNonIncrementalOperationCanBeAppendedToASignedSource`, and, for the
corrective CLI commands, `addBleedRefusesToWriteOverItsOwnInput`,
`rgbToCmykRefusesToWriteOverItsOwnInput` and
`repairRefusesToWriteOverItsOwnInput`; for the Action List,
`stepPlansCarryDeclaredSavePolicy` and
`executeRefusesWeakenedRequestedSavePolicy`.

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
the write-mode policy. A real signing fixture is carried in the repository at
`UnitTests/testdata/signatures/signed-incremental-base.pdf`, generated by
`scripts/qualification/make_signed_fixture.py` (the private key stays in a
temporary directory; only the signed PDF and its `manifest.json` are
committed). The byte-range claim is pinned by
`IncrementalSaveTest::signedFixtureIncrementalEditPreservesTheSignedByteRange`
and the cost claim by
`IncrementalSaveTest::appendCostScalesWithChangedDataNotFileSize`.

The structural and signature claims over the artifact that test emits are made
by external validators, not by Loop's own parser: the `reusable-linux.yml` job
installs `qpdf` and `poppler-utils`, runs `UnitTestsIncrementalSave` with
`LOOP_SAVE_POLICY_EVIDENCE_DIR` set, runs
`scripts/qualification/run_independent_validators.py` with `--claim structural
--claim signature`, and stores the evidence under
`docs/evidence/session-15-save-policy/`. A run on a host without those tools
records `incomplete` with `reason_code: validator-not-installed`, which never
counts as a pass (see [INDEPENDENT_VALIDATION.md](INDEPENDENT_VALIDATION.md)).
veraPDF conformance (`--claim standards`) remains an external release check
because this repository does not carry the veraPDF runtime.
