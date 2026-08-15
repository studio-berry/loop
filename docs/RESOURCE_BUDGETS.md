# Resource budgets for hostile PDF workloads

Loupe processing uses `pdf::PDFProcessingBudget` as a document-scoped,
cooperative safety boundary. The budget is shared by the Core document reader,
stream filters, document session caches, rendering, and preflight. Exhaustion
throws `pdf::PDFBudgetExceededException`; callers must treat it as an
incomplete operation and must not report a normal pass or successful output.

The conservative defaults bound input bytes, each decoded stream, cumulative
decoded bytes, decompression ratio, object count, recursive content depth,
render operations, rendered pixels, elapsed processing time, and named pools
for document-model, evidence-cache, raster/tile, undo, and rollback bytes.
Exhaustion reports `INCOMPLETE` with exact `budget.kind` (see
`getPDFBudgetKindName()`), never PASS.

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
unbounded or “disable all limits” mode. A budget failure is structured by
preflight as `checks[].budget` with `kind`, `limit`, `attempted`, and optional
`context`, alongside a `budget-exceeded` error and
`inspectionComplete: false`.

The budget is reset at the start of a reader operation and preflight run. A
session reset clears accounting and caches; document mutation still requires
the existing `PDFDocumentSession::invalidate()` call.
