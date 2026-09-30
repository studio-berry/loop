Category: added
Audience: developers and release qualifiers
Breaking-Change: no
Summary: Qualify hostile and production resource envelopes on hosted Linux and Windows runners (#19). `PdfTool benchmark --profile` adds a measured preflight phase, so a clean run reports a complete envelope; rendering stops within one page slice of an interrupt, and Windows honours CTRL_BREAK. A new resource-envelope qualification workflow generates a deterministic synthetic fixture bundle, runs the strict matrix with separate cancellation and reopen-after-cancel recovery probes and a hostile budget-exhaustion lane, and builds schema-2 evidence carrying the CI run id. Crashes, timeouts, and skipped workloads can never count as a passing envelope.
