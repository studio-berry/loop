# Editor crash/session recovery

**The recovery service described below does not exist in this tree.** It was
deleted by `2a19e2c1` ("delete Widgets libraries and plugin pack for Session 05
Issue 17"), which removed `LoupeLibGui/pdfrecoverymanager.{h,cpp}`. Everything
below is the contract to restore, not a description of shipped code.

`UnitTests/tst_recoverytest.cpp` is kept as the specification of the
source-identity and policy-clamp behaviour, but it includes
`pdfrecoverymanager.h`, a header that exists nowhere in the tree, and it is
registered in no CMake target. It therefore is not compiled and does not run.

The 0.3.0-A requirement ("crash recovery restores workspace/revision state
without presenting the recovered file as an approved production artifact") is
only half reachable today: the approval half is pinned by
`UnitTestsOperationHistory::noSavePathProducesAnApprovedOutputRecord` (no save
path records an approval or an approved output, so a recovered file cannot be
presented as approved), and the restore half is tracked by
[#575](https://github.com/studio-berry/loop/issues/575).

## Safety contract

- Dirty transitions are captured centrally from `PDFProgramController` and
  coalesced with a three-second debounce plus a thirty-second maximum interval.
- Serialization and hashing run through `QtConcurrent`; the UI mutation path
  only retains an immutable `PDFDocumentPointer` snapshot and revision number.
- A checkpoint writes a PDF and manifest to `.partial` paths, validates the
  payload SHA-256, then rotates the previous known-good generation before
  committing the new generation. Startup ignores partial files and can fall back
  to the previous validated pair.
- The original PDF is never written by recovery. Restored sessions are marked
  recovered and `Save` routes to `Save As` until the operator chooses an output.
- The manifest stores source identity (normalized-path hash, size, mtime, and a
  bounded prefix/suffix digest), revision, schema, and checkpoint metadata. The
  raw source path is retained only in the private local recovery store so the
  UI can identify the candidate; diagnostics do not include it.
- A per-session `QLockFile` prevents two Loop instances from claiming the same
  session. Stale locks are reclaimed only through Qt's stale-lock validation.

Encrypted documents fail closed until a checkpoint can preserve encryption
semantics safely. Signed documents are restored as working copies and retain a
manifest marker; restoration does not imply that an existing signature covers
the recovered edits.

## Lifecycle

Successful Save/Save As retires the recovery session only after the writer
returns success. Close + Discard retires it, while Cancel and failed Save keep
it. On startup, validated candidates are offered one at a time with Restore,
Discard, Open recovery folder, and Cancel actions. Source drift is shown and
restored as an independent dirty copy; missing sources remain recoverable.

Retention defaults to 14 days, 20 sessions, and 2 GiB. Cleanup runs after
classification and excludes active sessions. Invalid/stale candidates can be
discarded from the startup dialog without being opened.

No running test covers recovery: the `RecoveryTest` slots
`sourceIdentityDetectsReplacement` and `policyClampsUnsafeValues` are the
specification for source replacement/missing classification and policy clamping,
but their file is in no CMake target and does not compile against the current
tree. Restoring the service means restoring the manager and wiring that test up
in the same change, including the process-kill GUI coverage that belongs with
the GUI/E2E harness.
