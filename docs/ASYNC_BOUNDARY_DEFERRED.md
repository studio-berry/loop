# Async boundary: deferred work

Issue #144 requires that nothing expensive is reachable synchronously from an
input handler or a frame callback. For work that *has* an interactive caller,
that is proved by a migration: the caller submits through
`pdf::PDFJobScheduler`, and a test asserts it. For work that has no interactive
caller at all, there is nothing to migrate — and writing a migration plan for a
service that does not exist would be a plan nobody can execute.

This register is the other half of that answer. Each row names work that stays
where it is, what currently contains it, and the exact condition that turns it
into real work. The containment is `pdf::PDFThreadAffinity`: the interactive
thread is marked once in `LoupeEditor/main.cpp`, and every expensive service
entry opens with `requireNotInteractive()`. A path that has never been migrated
still cannot be reached from input handling without reporting itself.

A trigger firing is not a bug report against this document. It is the document
working.

## Register

| Work | Why deferred | Trigger that makes it real | First step when triggered |
| --- | --- | --- | --- |
| **AI / LLM** | No AI code exists in the tree. `PDFJobKind::Agent` and `PDFJobPriority::Agent` are reserved enumerators with one `switch` case each in `pdfjobscheduler.cpp`. There is no client, no provider, no prompt path, and no inference call anywhere. `agent-policy.json` and `scripts/agent/` are development tooling, not runtime. | A PR introduces any network inference call, **or** an `IAgentService` is proposed. | Add `LoupeLibInteraction/sources/agentservice.h` mirroring `IPreflightService`; submit as `PDFJobKind::Agent` / `PDFJobPriority::Agent`. Also add a sibling gate to `scripts/ci/check_unmanaged_async.py`: it scans for raw threads and `QtConcurrent`, and cannot see `QNetworkAccessManager`, so an un-scheduled network call would pass today. |
| **Thumbnail generation** | No thumbnail renderer exists. `UnitTests/tst_thumbnailsrenderertest.cpp` is on disk but referenced by no CMake file. `JOB_SCHEDULER.md` previously claimed this was migrated; it was not, and that row is now corrected. | A thumbnail strip lands in `LoupeEditor/qml`. | Route it through `PageSurfaceCoordinator` with `PDFJobKind::Thumbnail` / `PDFJobPriority::NearViewport`, and re-adopt the orphaned test rather than writing a second one. |
| **Image decode and optimize** | `pdfimage.cpp`, `pdfstreamfilters.cpp`, `pdfjbig2decoder.cpp` and `pdfccittfaxdecoder.cpp` are synchronous, but every current caller is already inside a scheduler worker (page render or export). The work is not on the interactive thread; it simply is not independently schedulable. | The `"image-decode"` affinity guard fires, in CI or in a development build. | Hoist the offending *caller* onto the scheduler. Do not make the decoder itself async — a decoder that owns its own concurrency is the second queue `jobsubmitter.h` exists to prevent. |
| **Font and metadata scan** | `pdffontintegrity.cpp` is synchronous and reached only from preflight, which now runs on a worker via `SchedulerPreflightService`. | The `"font-scan"` guard fires. | Same as image decode: move the caller, not the scanner. |
| **Batch analysis** | PageMaster and PdfTool only. No interactive surface can invoke it, so there is no interactive path to protect. | A batch surface appears in the editor. | `PDFJobKind::Batch` / `PDFJobPriority::Background`. |

## What did migrate

For contrast, and so this file is not read as a list of everything:

- **File I/O** — `DocumentFacade::beginOpen` / `beginSave`, scheduler-submitted,
  `JobRelay`-marshalled, generation-fenced with `Discard`.
- **Page rendering** — `PageSurfaceCoordinator`, revision-fenced through
  `RevisionFencedToken`, with admission bounds and terminal states.
- **Export** — `DocumentFacade`, `PDFJobKind::Export`.
- **Preflight** — `SchedulerPreflightService` for the editor,
  `PdfTool/pdftoolpreflight.cpp` for the CLI.
- **OCR** — `SchedulerOcrService` supplies the job boundary. The sidecar now runs
  on the one queue, cancellable and revision-fenced. There is no GUI OCR surface;
  that is a feature, not a boundary, and is out of scope for #144.

## Related

- `JOB_SCHEDULER.md` — the scheduler contract and the migration inventory.
- `INTERACTION_CONTRACT.md` — the thread-affinity rule and what may run on the
  interactive thread.
