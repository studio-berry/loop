# Deterministic repair diff

Loupe's repair-diff layer compares a source PDF with a serialized candidate
without treating indirect object numbers, xref offsets, compression filters, or
dictionary ordering as production changes. The Core API lives in
`LoupeLibCore/sources/pdfrepairdiff.h` and is usable by Editor, PdfTool, and
future repair transactions.

## Headless usage

```text
PdfTool repair-diff before.pdf candidate.pdf --console-format json
PdfTool repair-diff before.pdf candidate.pdf --render-dir repair-artifacts
```

The command returns the normal PdfTool envelope. `data.report` uses the
versioned `loupe.repair-diff` schema and includes source/candidate SHA-256
identities, page metrics, structural changes, warnings, and explicit incomplete
reasons. Use `--no-visual` for structural-only automation. PNG artifacts are
written only when `--render-dir` is selected.

Repair-specific allowances are explicit flags such as
`--allow-page-boxes`, `--allow-page-content`, `--allow-images`, and
`--allow-output-intent`. Unallowed high-impact changes are returned as findings;
cancelled, unsupported, budget-exceeded, or failed renders remain incomplete
and are never reported as zero change.

## Editor transaction

The bleed Editor fixup clones the open document, serializes and reopens a
private candidate, compares it using a fixed render policy, and presents
`RepairPreviewDialog` before approval. The candidate SHA-256 is checked again
before the atomic safe write, then the final bytes are hashed and reopened.
Post-fix preflight remains a separate validation step and is not collapsed into
the repair-diff status.
