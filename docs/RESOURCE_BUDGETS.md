# Resource budgets for hostile PDF workloads

Loupe processing uses `pdf::PDFProcessingBudget` as a document-scoped,
cooperative safety boundary. The budget is shared by the Core document reader,
stream filters, document session caches, rendering, and preflight. Exhaustion
throws `pdf::PDFBudgetExceededException`; callers must treat it as an
incomplete operation and must not report a normal pass or successful output.

The conservative defaults bound input bytes, each decoded stream, cumulative
decoded bytes, decompression ratio, object count, recursive content depth,
render operations, rendered pixels, and elapsed processing time. The existing
hard stream-filter ceilings remain active as a second line of defense.

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
