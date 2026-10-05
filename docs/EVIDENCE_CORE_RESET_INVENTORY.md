# L01-01 Evidence Core reset inventory

This is the source-to-contract disposition for [loop2 #15](https://github.com/studio-berry/loop2/issues/15), not a release qualification. The audited source is `origin/unstable` at `4388ca36db44cc187ba22031ef5e5aed8e3e2477` (2026-09-30); `git rev-parse origin/unstable` and `git ls-remote origin refs/heads/unstable` both returned this SHA at review time. The [L01 parent](https://github.com/studio-berry/loop2/issues/2) owns the module gate. Legacy Loop status is intake evidence, not a loop2 completion claim.

## Authority and catalog diff

The binding local inventory comes from [the generated architecture catalog](generated/architecture-catalog.json), [check catalog](generated/preflight-check-catalog.json), [coverage backlog](generated/preflight-coverage-backlog.json), [corpus map](generated/preflight-corpus-coverage.json), [proof lanes](../architecture/proof-lanes.yaml), and source at the SHA above. `python scripts/generate-architecture-catalogs.py --check` passed. Regeneration is therefore a zero-diff check against the committed catalogs at this SHA; this PR makes no catalog or check-registry change. SHA-256 identifies the exact catalog artifacts:

| Artifact | SHA-256 |
| --- | --- |
| `architecture-catalog.json` | `cfce8290a035aa36888949bd55d0446113ef8b2206e6907d8b47ac7688c8b3f0` |
| `preflight-check-catalog.json` | `0c1f3d09734a1d09250c907855aa89cff869b2bb5ed6569fc3a6e9051299ee46` |
| `preflight-coverage-backlog.json` | `cd14a670282ce888a89187641ac0d127ec4d71c2e104bde91efd0f0ca5d6914f` |
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
| PdfTool open/preflight process boundary | **Reuse** the Linux-first worker proof from legacy #618; **repair/audit** remaining privileged-host paths under [#20](https://github.com/studio-berry/loop2/issues/20). PdfTool supervisor and Core. | [`pdfworkerprotocol.h`](../PdfTool/pdfworkerprotocol.h) allowlists `ping`, `open`, `preflight`, `cancel`; [`pdfworkerclient.cpp`](../PdfTool/pdfworkerclient.cpp) maps worker failure/timeout to unavailable/incomplete; [`pdfworkersandbox.cpp`](../PdfTool/pdfworkersandbox.cpp), `UnitTestsPdfWorkerIsolation`, `scripts/ci/check_pdf_worker_isolation.py`. Windows runtime tests skip the Linux sandbox proof; [`editorhost.cpp`](../LoopEditor/editorhost.cpp) still constructs an in-process `PreflightEngine`. The open legacy #619 does not justify a replacement worker primitive. |
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

These are **all 18 `open` rows** in the [generated coverage backlog](generated/preflight-coverage-backlog.json) at the pinned SHA. Core preflight owns each. **Defer** means keep its current backlog row and its tracker reference; prioritize only after [#18](https://github.com/studio-berry/loop2/issues/18) defines the independent claim and [#16](https://github.com/studio-berry/loop2/issues/16) makes missing coverage visible in receipts. The two rows that remain `unfiled` are intentionally not requests to file a replacement primitive.

| Open gap ID | Priority | Disposition; tracker and legacy provenance |
| --- | --- | --- |
| `barcode-slug-braille` | P1 | **Defer**; backlog row, [#143](https://github.com/studio-berry/loop2/issues/143), legacy #604. |
| `devicen-per-colorant-ink-limit` | P1 | **Defer**; backlog row, [#142](https://github.com/studio-berry/loop2/issues/142), legacy #600. |
| `gwg-2022-2024-certificates` | P1 | **Defer**; backlog row, [#144](https://github.com/studio-berry/loop2/issues/144), legacy #664. |
| `imposition-and-reader-spreads` | P1 | **Defer**; backlog row, [#141](https://github.com/studio-berry/loop2/issues/141), legacy #603. |
| `pdfvt-variable-data` | P1 | **Defer**; backlog row, [#141](https://github.com/studio-berry/loop2/issues/141), legacy #605. |
| `bleed-raster-strip-depth` | P2 | **Defer**; backlog row, [#120](https://github.com/studio-berry/loop2/issues/120), legacy #47. |
| `color-mode-icc-alternate` | P2 | **Defer**; backlog row, [#113](https://github.com/studio-berry/loop2/issues/113); unfiled at the audited SHA. |
| `dieline-geometry` | P2 | **Defer**; backlog row, [#143](https://github.com/studio-berry/loop2/issues/143), legacy #604. |
| `font-glyph-coverage` | P2 | **Defer**; backlog row, [#114](https://github.com/studio-berry/loop2/issues/114); unfiled at the audited SHA. |
| `hidden-layers-ocmd` | P2 | **Defer**; backlog row, [#115](https://github.com/studio-berry/loop2/issues/115); unfiled at the audited SHA. |
| `ink-coverage-raster-tac` | P2 | **Defer**; backlog row, [#116](https://github.com/studio-berry/loop2/issues/116); unfiled at the audited SHA. |
| `invisible-content-breadth` | P2 | **Defer**; backlog row, [#117](https://github.com/studio-berry/loop2/issues/117); unfiled at the audited SHA. |
| `obscured-content-occlusion` | P2 | **Defer**; backlog row, [#118](https://github.com/studio-berry/loop2/issues/118); unfiled at the audited SHA. |
| `off-page-content-clipping` | P2 | **Defer**; backlog row, [#118](https://github.com/studio-berry/loop2/issues/118); unfiled at the audited SHA. |
| `transparency-rip-interaction` | P2 | **Defer**; backlog row, [#119](https://github.com/studio-berry/loop2/issues/119); unfiled at the audited SHA. |
| `white-overprint-renderer` | P2 | **Defer**; backlog row, [#119](https://github.com/studio-berry/loop2/issues/119), legacy #49. |
| `color-inventory-probe-depth` | P3 | **Defer**; backlog row, unfiled; reviewed deferral. |
| `thin-parts-raster-budget` | P3 | **Defer**; backlog row, unfiled; reviewed deferral; current failure is incomplete rather than a silent PASS. |

The seven `landed` and one `closed` backlog rows remain in the generated source and are **reuse/prove**, not new work: `corrupt-embedded-fonts`, `devicen-dieline-detection`, `hairline-and-thin-stroke-widths`, `nested-font-resources`, `output-intent-identity`, `thin-filled-parts`, `bleed-box-rewrite-only`, and `pdfx5-pdfa3-output`. Their exact state and `closed_by` are in the [backlog](generated/preflight-coverage-backlog.json).

## Review record

Reviewed artifacts: this inventory, the four generated catalogs ([architecture](generated/architecture-catalog.json), [checks](generated/preflight-check-catalog.json), [backlog](generated/preflight-coverage-backlog.json), [corpus](generated/preflight-corpus-coverage.json)) at `origin/unstable` = `4388ca36db44cc187ba22031ef5e5aed8e3e2477`, the [proof lanes](../architecture/proof-lanes.yaml), and the tracker references cited above. Review date: 2026-10-01. Reviewer: the repository owner (`mberrys`); acceptance of this inventory is recorded on [pull request #155](https://github.com/studio-berry/loop2/pull/155).

Decision: every disposition above is accepted as written. No row is an implementation mandate, so a `Defer` or `Reuse; prove` entry schedules no replacement primitive on its own. The two P3 rows that remain `unfiled` keep a reviewed deferral. This review corrected the stale `preflight-coverage-backlog.json` digest and the stale tracker references in the open-gap table, and re-pinned the audited SHA to `origin/unstable`.

Quality summary: this is a factual refresh, not new scope. It changes one digest, one pinned SHA, all eighteen tracker rows, and the review and proof records, and every changed value was re-derived from `docs/generated/` or a git command in this worktree. The dispositions and their verbs are unchanged, so no contract or implementation claim moved.

## Proof record and limits

Source SHA: `4388ca36db44cc187ba22031ef5e5aed8e3e2477` (`origin/unstable`). Fixture identity is the committed [preflight corpus map](generated/preflight-corpus-coverage.json) plus its `manifest` and `snapshot_dir` fields; no fixture or sealed output changed in this PR. These source commands ran on the pinned tree using the workspace Python runtime and returned these results:

```text
python scripts/generate-architecture-catalogs.py --check   # no output, exit 0
python -c "import hashlib,glob,os;[print(hashlib.sha256(open(p,'rb').read()).hexdigest(),os.path.basename(p)) for p in sorted(glob.glob('docs/generated/*.json'))]"   # four preflight digests, listed above
python -m unittest scripts.ci.test_preflight_check_catalog scripts.ci.test_preflight_corpus_coverage scripts.ci.test_check_independent_validation_gate -q   # 54 tests, OK
python scripts/ci/check_pdf_worker_isolation.py   # pdf worker isolation checks ok
python scripts/ci/check_independent_validation_gate.py   # release gate passed
```

The digest command produced the four values in the table above. The worker and independent-validation scripts are static contract checks. There is no `build*` directory in this worktree (`ls build*` found none), so native execution was unavailable without a configure/build step. This inventory does not claim runtime, installed-package, Linux sandbox, independent oracle, or release admission proof. The [L01 parent](https://github.com/studio-berry/loop2/issues/2) and [#21](https://github.com/studio-berry/loop2/issues/21) retain those gates.
