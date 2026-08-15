# Resource budgets for hostile PDF workloads

Loupe processing uses `pdf::PDFProcessingBudget` as a document-scoped,
cooperative safety boundary. The budget is shared by the Core document reader,
stream filters, document session caches, rendering, and preflight. Exhaustion
throws `pdf::PDFBudgetExceededException`; callers must treat it as an
incomplete operation and must not report a normal pass or successful output.

The conservative defaults bound input bytes, each decoded stream, cumulative
decoded bytes, decompression ratio, object count, recursive content depth,
render operations, rendered pixels, elapsed processing time, and evidence
records. Named `undo` and `rollback` pools exist with charge APIs; history
and artifact stores do not call them yet. The existing hard stream-filter
ceilings remain active as a second line of defense.

## Named pools

Each exhaustion reports the exact `budget.kind` and its pool. A budget
failure is **incomplete**, never PASS.

| Pool | Kinds |
|------|--------|
| `document-model` | `input-bytes`, `object-depth`, `recursive-content-depth`, `objects-visited`, `elapsed-time` |
| `decoded-streams` | `single-decoded-stream-bytes`, `cumulative-decoded-bytes`, `decompression-ratio` |
| `raster-tile` | `render-operations`, `render-pixels` |
| `evidence-cache` | `evidence-records` |
| `undo` | `undo-snapshots` |
| `rollback` | `rollback-artifacts` |

Preflight maps `PDFBudgetExceededException` to
`checks[].budget.{kind,pool,limit,attempted,context}` alongside a
`budget-exceeded` error and `inspectionComplete: false`.
`reducePreflightVerdict()` yields `incomplete`.

Synthetic exhaustion fixtures are generated in `UnitTestsBudgetExhaustion`
(nested objects, raster size, evidence records, elapsed clock). Do not
commit multi-GB binaries.

Under memory pressure, `PDFDocumentSession::shedPrefetchAndQuality()`
shrinks compile and stream cache caps. The job scheduler still reserves
an interaction slot when background work is saturated.

## Reader and session behavior

Reader input is accumulated in bounded chunks when the source is sequential;
it is never passed through an unbounded `readAll()` path. Parser object nesting
uses the configured object-depth budget, while content-stream recursion uses
the recursive-content-depth budget. Filter chains charge decoded output once
per produced stage; compressed source bytes are not misclassified as decoded
output, and the existing per-stream and ratio checks remain in force.

Trusted local workflows may explicitly choose a larger, finite profile:

```cpp
pdf::PDFProcessingLimits limits = pdf::PDFProcessingLimits::conservativeDefaults();
limits.maxInputBytes = 4LL * 1024 * 1024 * 1024;
limits.maxCumulativeDecodedBytes = 4LL * 1024 * 1024 * 1024;

pdf::PDFDocumentReader reader(progress, passwordCallback, false, false, limits);
// Or, for an existing document session:
session.setProcessingLimits(limits);
```

`PdfTool` and Editor integrations should expose this as an explicit named
profile or finite per-limit options, record the selected profile in their
diagnostics, and keep the conservative profile as the default. There is no
unbounded or “disable all limits” mode.

The budget is reset at the start of a reader operation and preflight run. A
session reset clears accounting and caches; document mutation still requires
the existing `PDFDocumentSession::invalidate()` call.
