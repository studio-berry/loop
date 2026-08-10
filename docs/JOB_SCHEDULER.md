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

The scheduler contract is landed in Core first so callers can migrate without
inventing incompatible job types. The following existing call sites are the
next migration set; they remain intentionally unchanged in this issue because
each needs an owner-specific result watcher and document-revision binding:

| Work | Existing owner | Scheduler kind | Default priority |
| --- | --- | --- | --- |
| Page and overlay rendering | `Pdf4QtLibGui`, `Pdf4QtLibWidgets` | `Rendering` | `VisiblePage` |
| Preflight and fixups | Editor / PdfTool | `Preflight` or `Export` | `Operator` |
| OCR and indexing | Editor plugins / Core | `OCR` | `Background` |
| PageMaster export | `Pdf4QtPageMaster` | `Export` | `Operator` |
| Thumbnail generation | `Pdf4QtLibWidgets` | `Thumbnail` | `NearViewport` |
| Batch analysis | PageMaster / PdfTool | `Batch` | `Background` |
| Agent context work | future agent surface | `Agent` | `Agent` |

This migration boundary is deliberate: the scheduler provides the shared
arbitration contract, while subsequent caller changes must preserve each
surface's typed result and UI lifecycle. A source audit can use the table to
reject new unmanaged long-running work and to track the remaining conversions.

## Verification

`UnitTestsJobScheduler` covers stable priority ordering, terminal
cancellation, measured cancellation latency, stale-revision discard, progress,
metadata, and trace visibility. The test uses one worker so priority behavior
is deterministic and does not depend on machine concurrency.
