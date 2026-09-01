# Loupe job scheduler

`PDFJobScheduler` is the application-wide Core submission boundary for work
that can outlive the initiating interaction. Applications should use
`PDFJobScheduler::global()` (or an explicitly owned scheduler in tests) rather
than starting an independent long-running `QtConcurrent`, `QThreadPool`, or
`std::thread` operation.

## Job contract

Every `PDFJobSpec` carries:

- `jobId`, artifact identity, and document key/revision;
- the operation or check identifier;
- one of the six ordered priority classes;
- a `PDFJobKind` identifying rendering, preflight, OCR, export, thumbnails,
  batch, or agent work;
- a `PDFProcessingLimits` budget and progress model; and
- an explicit stale-result policy.

The work callback receives a `PDFJobContext`. It exposes the shared
`PDFOperationControl` cancellation view, a processing budget, progress
reporting, a result summary, and an optional output artifact. A caller may
present output only after the scheduler reports `Succeeded`. `Cancelled`,
`Failed`, and `Stale` are terminal non-success states and must not be treated
as completed output.

## Priority and cancellation

The scheduler uses a fixed worker count and a stable priority queue. Lower
numeric priority values run first; jobs at the same priority retain submission
order. With more than one worker, one worker slot is reserved for interaction,
visible-page, near-viewport, and operator jobs so a saturating background queue
cannot consume all capacity needed to produce visible work. Cancellation is
cooperative because PDF processing primitives already
poll `PDFOperationControl`. `cancel()` records the request and the terminal
snapshot reports `Cancelled` even if a callback returns normally afterward.
`cancellationLatencyMs` and the trace event are the measurement points for
the cancellation-latency benchmark.

Queued cancellation is finalized without invoking the callback. A running
callback must poll `PDFJobContext::isCancellationRequested()` or pass
`operationControl()` to the underlying Core operation and abandon any partial
result.

## Revision freshness

The document authority should call `setCurrentRevision(documentKey,
revision)` whenever the accepted document revision changes. A queued or
running job whose revision no longer matches is finalized as `Stale` when its
policy is `Discard`; the callback is not run for a stale queued job. This keeps
late tile, overlay, preflight, OCR, and export results from being presented
for a newer document revision.

## Migration inventory

The scheduler contract is landed in Core. Callers migrate onto `PDFJobScheduler`
with document-revision binding. Inventory:

| Work | Existing owner | Scheduler kind | Default priority | Status |
| --- | --- | --- | --- | --- |
| Page and overlay rendering | `LoupeLibQuick`, `LoupeLibCore` | `Rendering` | `VisiblePage` | **page compile and text layout migrated**; remaining overlay tiles stay on `PDFExecutionPolicy` |
| Preflight and fixups | Editor / PdfTool | `Preflight` or `Export` | `Operator` | **PdfTool `preflight` migrated**; Editor preflight migrated by `SchedulerPreflightService` (issue #144) |
| OCR and indexing | Editor plugins / Core | `OCR` | `Background` | **migrated (job boundary only; no GUI surface)** via `SchedulerOcrService` (issue #144) |
| Page production export | `PdfTool`, `LoupeLibCore` | `Export` | `Operator` | **migrated** |
| Thumbnail generation | `LoupeLibQuick`, `LoupeLibCore` | `Thumbnail` | `NearViewport` | remaining -- no thumbnail renderer exists in the tree; see `ASYNC_BOUNDARY_DEFERRED.md` |
| Page production preview | `PdfTool`, `LoupeLibCore` | `Rendering` | `NearViewport` | **migrated** (revision-fenced via `PDFJobScheduler`) |
| Batch analysis | `PdfTool`, `LoupeLibCore` | `Batch` | `Background` | remaining; see `ASYNC_BOUNDARY_DEFERRED.md` |
| Agent context work | future agent surface | `Agent` | `Agent` | deferred -- no AI code exists; see `ASYNC_BOUNDARY_DEFERRED.md` |

This migration boundary is deliberate: the scheduler provides the shared
arbitration contract, while subsequent caller changes must preserve each
surface's typed result and UI lifecycle. A source audit can use the table to
reject new unmanaged long-running work and to track the remaining conversions.

`scripts/ci/check_unmanaged_async.py` is the CI guard for that boundary. It
currently reports one pre-existing `QtConcurrent::run` call site
(`LoupeLibCore/sources/pdfdiff.cpp`) as known migration debt and fails on any new or multiplied unmanaged launch. The
allowlist is a containment measure, not acceptance of #238; the issue remains
open until those product paths are migrated and cancellation/stale-result
evidence is recorded on both supported desktop platforms.

## Verification

`UnitTestsJobScheduler` covers stable priority ordering, terminal
cancellation (including Export/Preflight operator jobs), measured cancellation
latency, stale-revision discard, progress, metadata, and trace visibility.
`PageMasterExportTest::cancel_midOutput_beforeWrite_writesNothing` submits
export through `PDFJobScheduler` and asserts the snapshot is not `Succeeded`.

Work with no interactive caller is not left unguarded. `pdf::PDFThreadAffinity`
marks the interactive thread at startup, and each expensive service entry calls
`requireNotInteractive()`, so a path that has never been migrated still cannot
be reached synchronously from input handling. What remains, and what makes each
item real work, is registered in `ASYNC_BOUNDARY_DEFERRED.md`.
