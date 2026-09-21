# Editor crash/session recovery

**The recovery service is restored at its value level only.** What exists in
this tree is `LoopLibCore/sources/pdfrecoverymanager.{h,cpp}`: the policy and its
clamp (`RecoveryPolicy`, `clampRecoveryPolicy`), the source identity and its
classification (`RecoverySourceIdentity`, `inspectRecoverySource`,
`classifyRecoverySource`), and the status and candidate types those need. That
surface is registered Core code, and so is its test: the `RecoveryTest` slots in
`UnitTests/tst_recoverytest.cpp` build as the `UnitTestsRecovery` target, listed
in `UnitTests/CMakeLists.txt` and in `agent-policy.json`.

What does not exist yet: the recovery manager itself, the per-session
`QLockFile` claim, the checkpoint write with its generation rotation, the
retention sweep, and the Editor wiring that captures dirty transitions and
offers candidates at startup. The service was deleted by `2a19e2c1` ("delete
Widgets libraries and plugin pack for Session 05 Issue 17"), which removed
`pdfrecoverymanager.{h,cpp}` along with the GUI library that hosted it; the
branch name in that commit records the old library name. The historical path is
recoverable without depending on this page:
`git log --all --diff-filter=D --name-only -- '*pdfrecoverymanager*'`. Everything
below remains the contract to restore, not a description of shipped code.

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
- Serialization and hashing run off the UI mutation path, which only retains an
  immutable `PDFDocumentPointer` snapshot and revision number. The sanctioned
  mechanism is the job scheduler, not `QtConcurrent`, whose remaining launches
  `scripts/ci/check_unmanaged_async.py` pins at zero.
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

The `RecoveryTest` slots `sourceIdentityDetectsReplacement` and
`policyClampsUnsafeValues` are the only executing specification of this contract,
and they run now: `UnitTestsRecovery` is app-less (`QTEST_APPLESS_MAIN`, no
application object and no event loop). They cover source replacement/missing
classification and policy clamping, and nothing else. Nothing executes the
manager yet: the checkpoint write and rotation, the lock, restore/discard, and
the retention sweep have no coverage, and the process-kill GUI coverage that
belongs with the GUI/E2E harness does not exist.
