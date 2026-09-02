# Upstream divergence register

Loop is a product fork of [JakubMelka/PDF4QT](https://github.com/JakubMelka/PDF4QT).
This register lists **behavioral divergences in the shared PDF
parser, writer, or renderer**. Cosmetic Loop-only code, branding,
Editor plugins, PdfTool commands, and documentation do **not** belong
here.

Before merging an authorized upstream sync into `dev`, grep this file
and re-run the mapped tests. A clean merge is not verification.

## Policy

- Pull upstream only when explicitly requested (`docs/REPO_MAP.md`).
- Prefer upstream engine fixes unless a current Loop ADR or issue
  says otherwise.
- If Loop must keep a parser/writer/renderer change, add a row here
  in the same change set.

## Register

| Area | Loop behavior | Upstream | Tests | Notes |
|------|----------------|----------|-------|-------|
| Processing budgets | `PDFProcessingBudget` bounds decode, raster, and graph work; exhaustion is incomplete | No equivalent named pools | `UnitTestsProcessingBudget`, `UnitTestsBudgetExhaustion` | #242 / #243 |
| Plugin ABI | Manifest ABI/capabilities inspected before `QPluginLoader::instance()`; packaged plugin dir only | Loads any plugin after `load()` | `UnitTestsPluginAbi` | #269 |
| Revision fence | `PDFRevisionIdentity` discards stale async/cache results | Viewer caches are not revision-fenced | `UnitTestsDocumentSession`, `UnitTestsJobScheduler`, `UnitTestsRevisionStress` | #236 |
| Incremental save | Source digest mismatch refuses a silent rewrite | Writer may overwrite | `UnitTestsIncrementalSave` | #239 |
| Render fidelity | Standard rendering reports cached overprint content as an explicit approximation; preflight and separation policies prohibit approximation | Standard renderer has no fidelity diagnostic | `UnitTestsOverprint` | #49 / #52 |

Rows are added when Loop changes parser, xref/writer, or raster
behavior relative to upstream. Empty product-only work stays out.
