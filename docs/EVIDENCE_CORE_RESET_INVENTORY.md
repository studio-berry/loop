# L01-01 Evidence Core reset inventory

This is the source-to-contract disposition for [loop2 #15](https://github.com/studio-berry/loop2/issues/15), not a release qualification. The audited source is `origin/dev` at `5c8366a33e07597f5213be8da594e3120f73ed69` (2026-09-24); `git ls-remote origin refs/heads/dev` matched the local tracking ref before the topic branch was made. The [L01 parent](https://github.com/studio-berry/loop2/issues/2) owns the module gate. Legacy Loop status is intake evidence, not a loop2 completion claim.

## Authority and catalog diff

The binding local inventory comes from [the generated architecture catalog](generated/architecture-catalog.json), [check catalog](generated/preflight-check-catalog.json), [coverage backlog](generated/preflight-coverage-backlog.json), [corpus map](generated/preflight-corpus-coverage.json), [proof lanes](../architecture/proof-lanes.yaml), and source at the SHA above. `python scripts/generate-architecture-catalogs.py --check` passed. Regeneration is therefore a zero-diff check against the committed catalogs at this SHA; this PR makes no catalog or check-registry change. SHA-256 identifies the exact catalog artifacts:

| Artifact | SHA-256 |
| --- | --- |
| `architecture-catalog.json` | `cfce8290a035aa36888949bd55d0446113ef8b2206e6907d8b47ac7688c8b3f0` |
| `preflight-check-catalog.json` | `0c1f3d09734a1d09250c907855aa89cff869b2bb5ed6569fc3a6e9051299ee46` |
| `preflight-coverage-backlog.json` | `de68c626b180905d59aa73e94b41d04ecff6ca3b761fb912d812ba007a36162d` |
| `preflight-corpus-coverage.json` | `aec3acc671cf8ccb5611e2977b03b1268b5eb4fdf09adca9bbad230ffe65ee91` |

The catalog has **22 registered checks**: 5 `covered`, 17 `partial`, and no `not_covered` check row. Its separate backlog has 26 rows: 18 `open`, 7 `landed`, and 1 `closed`. `covered` is limited to the catalog's named check and corpus scope; it is not a standards certificate. The [coverage matrix](PREFLIGHT_COVERAGE_MATRIX.md) defines the claim and the fixture rule.

## Source-to-contract dispositions

`Prove` means keep the primitive and obtain current exact-SHA behavioral evidence. `Reuse` means the source contract is already present and no replacement is justified. `Repair` names a bounded next gate. `Defer` keeps a known limitation visible without treating a legacy issue as an implementation mandate. Every row's owner is LoopLibCore unless another owner is named.

| Inherited capability or boundary | Disposition and owner | Source, contract, and proof |
| --- | --- | --- |
| Profile import, variable binding, check registry, per-check status, and coverage scope | **Reuse; prove** under L01-02. Core preflight. | [`preflightengine.cpp`](../LoopLibCore/sources/preflightengine.cpp), [ADR-002](adr/adr-002-preflight-engine-orchestrator.md), [check catalog](generated/preflight-check-catalog.json); `UnitTestsPreflightEngine`, `UnitTestsPreflightChecks`, `UnitTestsPreflightProfileResolver`, `UnitTestsPreflightCorpus`. |
| Evidence graph, report JSON, and portable bundle | **Reuse** existing evidence fields; **repair** complete revision-bound receipt and limitation admission in [#16](https://github.com/studio-berry/loop2/issues/16). Core evidence. | [`pdfevidencegraph.cpp`](../LoopLibCore/sources/pdfevidencegraph.cpp), [`pdfpreflightevidencebundle.cpp`](../LoopLibCore/sources/pdfpreflightevidencebundle.cpp), [bundle contract](PREFLIGHT_EVIDENCE_BUNDLE.md); `UnitTestsEvidenceGraph`, `UnitTestsPreflightEngine`. A bundle is not itself proof that every required inspection ran. |
| Artifact, document revision, and profile identity | **Reuse; prove** exact request/result binding under [#16](https://github.com/studio-berry/loop2/issues/16) and [#17](https://github.com/studio-berry/loop2/issues/17). Core identity. | [`pdfartifactidentity.h`](../LoopLibCore/sources/pdfartifactidentity.h), [revision contract](REVISION_CONTEXT.md), [ADR-001](adr/adr-001-pdf-document-session.md); `UnitTestsIdentitySeparation`, `UnitTestsDocumentSession`, `UnitTestsRevisionStress`. |
| Canonical Pass/Fail/Incomplete/Error reducer and certificate gate | **Reuse; prove** all four states and no zero-finding budget PASS under [#16](https://github.com/studio-berry/loop2/issues/16). Core verdict. | [`pdfpreflightverdict.cpp`](../LoopLibCore/sources/pdfpreflightverdict.cpp), [verdict contract](PREFLIGHT_VERDICT.md); `UnitTestsPreflightVerdict`, `UnitTestsPreflightEngine`. `PreflightResult::pass` is derived compatibility data. |
| Fixed-capacity job scheduler, cancellation, stale-result discard | **Reuse** the existing scheduler; **repair** producer/result fencing under [#17](https://github.com/studio-berry/loop2/issues/17). Core scheduling. | [`pdfjobscheduler.cpp`](../LoopLibCore/sources/pdfjobscheduler.cpp), [scheduler contract](JOB_SCHEDULER.md); `UnitTestsJobScheduler`, `UnitTestsRevisionStress`, `scripts/ci/check_unmanaged_async.py`. Caller coverage is not complete merely because the scheduler exists. |
| Parser, reader, renderer, session, processing and resource budgets | **Reuse** Core primitives; **prove** hostile and production envelopes under [#19](https://github.com/studio-berry/loop2/issues/19). Core PDF. | [`pdfdocumentreader.cpp`](../LoopLibCore/sources/pdfdocumentreader.cpp) calls [`pdfparser.cpp`](../LoopLibCore/sources/pdfparser.cpp); [`pdfrenderer.cpp`](../LoopLibCore/sources/pdfrenderer.cpp) and [budget contract](RESOURCE_BUDGETS.md) bound work. `UnitTestsProcessingBudget`, `UnitTestsResourceBudget`, `UnitTestsBudgetExhaustion`, `UnitTestsBudgetCorpus` are mapped tests. The unbudgeted cumulative `PDFFunction::createFunction()` path remains an explicit deferred contract-level gap in that document. |
| PdfTool open/preflight process boundary | **Reuse** the Linux-first worker proof from [legacy #618](https://github.com/studio-berry/loop/issues/618); **repair/audit** remaining privileged-host paths under [#20](https://github.com/studio-berry/loop2/issues/20). PdfTool supervisor and Core. | [`pdfworkerprotocol.h`](../PdfTool/pdfworkerprotocol.h) allowlists `ping`, `open`, `preflight`, `cancel`; [`pdfworkerclient.cpp`](../PdfTool/pdfworkerclient.cpp) maps worker failure/timeout to unavailable/incomplete; [`pdfworkersandbox.cpp`](../PdfTool/pdfworkersandbox.cpp), `UnitTestsPdfWorkerIsolation`, `scripts/ci/check_pdf_worker_isolation.py`. Windows runtime tests skip the Linux sandbox proof; [`editorhost.cpp`](../LoopEditor/editorhost.cpp) still constructs an in-process `PreflightEngine`. The open [legacy #619](https://github.com/studio-berry/loop/issues/619) does not justify a replacement worker primitive. |
| Independent standards/rendering validation | **Reuse** the validation harness; **prove** independent oracle outputs and fidelity claims under [#18](https://github.com/studio-berry/loop2/issues/18). Core qualification. | [`check_independent_validation_gate.py`](../scripts/ci/check_independent_validation_gate.py), [independent evidence schema](schemas/independent-validation-evidence.schema.json), [coverage matrix](PREFLIGHT_COVERAGE_MATRIX.md), `UnitTestsConversionOracle`. The source gate checks presence/guards; it is not a current installed-runtime oracle result. |
| Cross-platform exact-SHA admission | **Defer** release admission to [#21](https://github.com/studio-berry/loop2/issues/21). Core qualification with CI owners. | [Proof lanes](../architecture/proof-lanes.yaml) bind `linux-build` and `windows-build`; [parent exit gate](https://github.com/studio-berry/loop2/issues/2) requires one exact-SHA packet. No such packet is asserted by this inventory. |

The accepted/implemented **inherited** ADR coverage relevant to this slice is [ADR-001](adr/adr-001-pdf-document-session.md) for session/revision authority, [ADR-002](adr/adr-002-preflight-engine-orchestrator.md) for the Core registry, and [ADR-011](adr/adr-011-architecture-contracts-d1-d5.md) for canonical digests and governed output identity. Their `Last-verified` SHAs belong to the copied Loop history; current loop2 behavior still needs the proof above. The [worker-isolation ADR](https://app.notion.com/p/3dc9cb079ddb812b9f1fd668196818c6) is marked **proposed**, while legacy #618 is the narrower accepted gate. The [master roadmap](https://app.notion.com/p/3bb9cb079ddb80c4a15feaa98f963f4c) is planning context; its L01 receipt sketch does not create a new public schema.

## Registered check disposition

Each ID below is present in [`PreflightEngine::registerBuiltInChecks()`](../LoopLibCore/sources/preflightengine.cpp) and the [generated check catalog](generated/preflight-check-catalog.json). Core preflight owns every row. **Reuse; prove** means retain the check and its catalog limitation, then run the mapped engine/check/corpus tests on an exact candidate SHA. A partial row remains partial even if those tests pass; the open gaps below govern work beyond that measured scope.

| Catalog coverage | Check IDs | Disposition |
| --- | --- | --- |
| `covered` | `embedded-fonts`, `image-resolution`, `output-intent`, `page-size`, `trim` | **Reuse; prove** each named scope and its fixture evidence. |
| `partial` | `bleed`, `color-inventory`, `color-mode`, `conformance-claims`, `content-bleed`, `dieline`, `font-integrity`, `hidden-layers`, `ink-coverage`, `invisible-content`, `obscured-content`, `off-page-content`, `processing-steps`, `thin-parts`, `thin-strokes`, `transparency-risk`, `white-overprint` | **Reuse; prove** the bounded detection and preserve each catalog limitation. |

## Open legacy gap disposition

These are **all 18 `open` rows** in the [generated coverage backlog](generated/preflight-coverage-backlog.json) at the pinned SHA. Core preflight owns each. **Defer** means keep its current backlog row and issue reference, if any; prioritize only after [#18](https://github.com/studio-berry/loop2/issues/18) defines the independent claim and [#16](https://github.com/studio-berry/loop2/issues/16) makes missing coverage visible in receipts. An `unfiled` row is intentionally not a request to file a replacement primitive.

| Open gap ID | Priority | Disposition; evidence/legacy owner |
| --- | --- | --- |
| `barcode-slug-braille` | P1 | **Defer**; backlog row, [Loop #604](https://github.com/studio-berry/loop/issues/604). |
| `devicen-per-colorant-ink-limit` | P1 | **Defer**; backlog row, [Loop #600](https://github.com/studio-berry/loop/issues/600). |
| `gwg-2022-2024-certificates` | P1 | **Defer**; backlog row, [Loop #664](https://github.com/studio-berry/loop/issues/664). |
| `imposition-and-reader-spreads` | P1 | **Defer**; backlog row, [Loop #603](https://github.com/studio-berry/loop/issues/603). |
| `pdfvt-variable-data` | P1 | **Defer**; backlog row, [Loop #605](https://github.com/studio-berry/loop/issues/605). |
| `bleed-raster-strip-depth` | P2 | **Defer**; backlog row, [Loop #47](https://github.com/studio-berry/loop/issues/47). |
| `color-mode-icc-alternate` | P2 | **Defer**; backlog row, unfiled. |
| `dieline-geometry` | P2 | **Defer**; backlog row, [Loop #604](https://github.com/studio-berry/loop/issues/604). |
| `font-glyph-coverage` | P2 | **Defer**; backlog row, unfiled. |
| `hidden-layers-ocmd` | P2 | **Defer**; backlog row, unfiled. |
| `ink-coverage-raster-tac` | P2 | **Defer**; backlog row, unfiled. |
| `invisible-content-breadth` | P2 | **Defer**; backlog row, unfiled. |
| `obscured-content-occlusion` | P2 | **Defer**; backlog row, unfiled. |
| `off-page-content-clipping` | P2 | **Defer**; backlog row, unfiled. |
| `transparency-rip-interaction` | P2 | **Defer**; backlog row, unfiled. |
| `white-overprint-renderer` | P2 | **Defer**; backlog row, [Loop #49](https://github.com/studio-berry/loop/issues/49). |
| `color-inventory-probe-depth` | P3 | **Defer**; backlog row, unfiled. |
| `thin-parts-raster-budget` | P3 | **Defer**; backlog row, unfiled; current failure is incomplete rather than a silent PASS. |

The seven `landed` and one `closed` backlog rows remain in the generated source and are **reuse/prove**, not new work: `corrupt-embedded-fonts`, `devicen-dieline-detection`, `hairline-and-thin-stroke-widths`, `nested-font-resources`, `output-intent-identity`, `thin-filled-parts`, `bleed-box-rewrite-only`, and `pdfx5-pdfa3-output`. Their exact state and `closed_by` are in the [backlog](generated/preflight-coverage-backlog.json).

## Proof record and limits

Source SHA: `5c8366a33e07597f5213be8da594e3120f73ed69`. Fixture identity is the committed [preflight corpus map](generated/preflight-corpus-coverage.json) plus its `manifest` and `snapshot_dir` fields; no fixture or sealed output changed in this PR. These source commands passed on the pinned tree using the workspace Python runtime:

```text
python scripts/generate-architecture-catalogs.py --check
python -m unittest scripts.ci.test_preflight_check_catalog scripts.ci.test_preflight_corpus_coverage scripts.ci.test_check_independent_validation_gate -q  # 44 passed
python scripts/ci/check_pdf_worker_isolation.py
python scripts/ci/check_independent_validation_gate.py
```

The worker and independent-validation scripts are static contract checks. `ctest --test-dir build -N` found the focused C++ test registrations but no executables in the existing build, so native execution was unavailable without a build/configure step. This inventory does not claim runtime, installed-package, Linux sandbox, independent oracle, or release admission proof. The [L01 parent](https://github.com/studio-berry/loop2/issues/2) and [#21](https://github.com/studio-berry/loop2/issues/21) retain those gates.
