# ADR-004: PageMaster batch manifest and atomic per-output writes

**Status:** implemented
**Implemented-at:** fed0e6a30b3e67e2f39ecd941dde728b4e72a561
**Last-verified:** 2026-09-20 @ dbb1c32427884855a9a3efacbcc3de4e4f487952
**Superseded-by:** none
**Date:** 2026-07-20
**Amended:** 2026-09-20 (as-built correction; see "Amendment")
**Deciders:** MIC-309 / Cycle 2 sprint plan

## Context

`PDFPageMasterExport::run()` (ADR-003) writes outputs sequentially and lists
completed paths in `writtenFiles`, but a mid-batch failure or process kill can
leave torn PDFs on disk with no durable record of which outputs finished.
MIC-309 requires atomic per-output commits and a partial-result manifest that
enables resume or cleanup.

## Decision (as built)

- **Manifest file:** `.loop-batch.json` in the directory of the first output path
  in the batch (`pdfpagemasterexport.cpp:110`), or the caller's
  `PDFPageMasterExportJob::manifestPath` when set. One manifest per export
  invocation; `batch_id` is a UUID string generated at batch start.
- **Manifest schema (version 3,** `pdfpagemasterexport.cpp:60`**):**

```json
{
  "schema_version": 3,
  "batch_id": "<uuid>",
  "outputs": [
    {
      "path": "<absolute or as-given output path>",
      "status": "pending|written|failed",
      "error": "<present only while the entry is failed>"
    }
  ],
  "source_identities": { "<document index>": { "sha256": "...", "size": 0, "mediaType": "...", "logicalName": "..." } },
  "effective_profile_digest": "<sha256, empty without a preflight gate>",
  "export_config_digest": "<sha256 over outputs, assembly topology, settings and identities>",
  "action_list": { "<present only when the job carried an Action List>" }
}
```

  Per output the code additionally records `bleed_report`, `preflight`,
  `action_list_result` and `governed` where those stages ran
  (`pdfpagemasterexport.cpp:984`, `:1016`, `:1119`, `:1133`).
- **Statuses are exactly three** (`pdfpagemasterexport.cpp:62-64`): `pending`,
  `written`, `failed`. **There is no `skipped` status** and there never was one.
  An output a resuming run decides not to touch simply keeps its `written` entry.
- **Atomic write:** each output goes through `PDFSafeFileWriter::writeDevice()`,
  which opens a `QSaveFile` with `setDirectWriteFallback(false)`, hands the open
  device to a producer, and calls `commit()` only after the producer succeeded
  (`pdfsafefilewriter.cpp:63`). There is no `<finalPath>.<pid>.partial` name and no
  `QFile::rename`; no partial-rename code exists. The document is serialized once
  and those exact bytes are what reaches the writer
  (`pdfpagemasterexport.cpp:1782`).
  - A process killed between the producer's write and `commit()` leaves Qt's own
    staging file behind, named `<finalPath>.XXXXXX` with six random alphanumerics.
    Measured for issue #44: the residue appeared as `staging-3.pdf.OGzKlZ`,
    `staging-3.pdf.aosLnQ` and `staging-3.pdf.wWeIfS`, and in each case held 0
    bytes - the writer had not yet flushed the payload. The residue never appears
    **at** a final path. What should clean it up is an open question, not a
    guarantee this ADR makes.
- **Manifest updates:** after every attempt, success or failure, the manifest is
  rewritten through the same atomic writer. The terminal `written` stamp for the
  final output is `pdfpagemasterexport.cpp:1910`; an output whose write failed is
  stamped at `:1841`.
- **The manifest is retained after a successful batch.** Nothing deletes it and no
  completion marker replaces it: the terminal state on disk is `schema_version: 3`
  with every output `written`.
- **Resume** (`job.resume` true and a manifest beside the first output):
  - an output is left alone only when its entry is `written` **and** its file still
    exists on disk (`pdfpagemasterexport.cpp:1177`); a `written` entry whose file
    was deleted is produced again
  - `pending` and `failed` entries are retried
  - a manifest whose `schema_version` differs, whose output list differs, or whose
    `export_config_digest` differs from the job fails closed, and the rejected
    manifest is left exactly as it was
  - **no manifest at all is not an error:** the run proceeds exactly as
    `job.resume == false`, writing a fresh manifest
    (`pdfpagemasterexport.cpp:1356`)
- **The manifest path belongs to the planned-output conflict set**
  (`pdfpagemasterexport.cpp:1324`, `:1359`): a pre-existing manifest at the planned
  path is a conflict when overwriting is not allowed, so a second batch into the
  same directory without overwrite is refused before it writes anything.
- **The default manifest name is per directory, not per batch.** Two batches
  exporting into one directory share `.loop-batch.json`, and the later batch's
  state wins. There is no lock; the register of that aliasing is the test slot
  `manifest_twoBatchesShareDefaultNameInOneDirectory_aliased`. A caller that needs
  independent state sets `manifestPath`.
- **API additions (unchanged from the original decision):**
  - `PDFPageMasterExportJob::resume` (default `false`)
  - `PDFPageMasterExportJob::manifestPath` (optional override; default derived from
    the first output's directory)
  - `PDFPageMasterExportResult::manifestPath` and `::manifest`
- **Test seams** (documented because the tests depend on them, empty in
  production): `PDFPageMasterExportJob::manifestPersist` substitutes manifest
  persistence, and `PDFPageMasterExportJob::beforeOutputCommit` runs for one output
  - keyed by that output's own path - after its bytes have been handed to the
  atomic writer and before the commit that publishes them.

## Amendment (2026-09-20) - what this ADR used to claim

The decision text above replaced a description of a design that was never the
shipped one. Issue #44's rule is that the ADR wins or is amended, and the code was
measured first, so the drifted claims are superseded deliberately here rather than
quietly rewritten.

| Original claim | As built |
| --- | --- |
| schema version 1 | schema version **3** (`pdfpagemasterexport.cpp:60`) |
| statuses `pending\|written\|failed\|skipped` | `pending\|written\|failed`; `skipped` does not exist |
| `<finalPath>.<pid>.partial` written with `PDFDocumentWriter`, then `QFile::rename`; a failed rename removes the partial | `QSaveFile` open / producer / `commit()`, with direct write fallback disabled (`pdfsafefilewriter.cpp:63`) |
| manifest rewritten as `.loop-batch.json.<pid>.partial` then renamed | the same `QSaveFile` helper as any other output |
| `skipped` entries are never rewritten | there are no `skipped` entries; a `written` entry is left alone only while its file exists |
| (unstated) manifest lifecycle after a successful batch | retained with every output `written`; no delete, no completion marker |
| PageMaster UI auto-enables resume when a manifest exists in the export directory | no caller in this tree includes `pdfpagemasterexport.h` - only the library and `UnitTestsPageMasterExport` - so there is no UI surface for it here; the headless API is the whole contract |

One original promise survives and is now measured instead of asserted: no reader
ever sees a partial file **at a final output path**. A hard exit inside the
publishing window leaves no file at the final path of the interrupted output, the
two outputs committed before it remain valid PDFs, and the interrupted batch
resumes to a complete one.

## Consequences

- Mid-batch kill leaves only complete outputs plus a manifest identifying state.
- Resume is headless-testable via `UnitTestsPageMasterExport`, and every claim
  above has a slot: `multiOutput_manifestRecordsFailureAndPendingOutputs`,
  `atomicWrite_leavesNoPartialFiles`, `manifest_persistedWithWrittenStatuses`,
  `manifest_persistFailure_removesNewOutput`, `manifest_corruptResumeFailsClosed`,
  `manifest_concurrentBatchesHaveIndependentState`,
  `processKill_afterAtomicOutputLeavesNoPartialFile`, the six `resume_*` slots, and
  the issue #44 slots that pin the staging-window kill, the failed-entry retry, the
  deleted-file re-run, the absent manifest, the unreadable and empty-outputs
  manifests, the shared default name and the success-path defaults.
- MIC-312 batch preflight can key off the same manifest paths.
- A directory two batches export into shares one manifest; callers that need
  independent state set `manifestPath` per batch.
