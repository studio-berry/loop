# Editor crash/session recovery

Loupe protects unsaved Editor work with a private, bounded recovery store. The
store is owned by `PDFRecoveryManager` in `LoupeLibGui` and is attached to the
single-document Editor session.

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
- A per-session `QLockFile` prevents two Loupe instances from claiming the same
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

`UnitTestsRecovery` covers source replacement/missing classification and policy
clamping. The service boundaries are deterministic and ready for injected fake
clock/filesystem crash-point tests; process-kill GUI coverage belongs with the
GUI/E2E harness tracked separately.
