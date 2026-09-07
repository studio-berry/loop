# Semantic trust engine — grouped Session 1 acceptance matrix

This is the qualification ledger for the grouped Session 1 pass over issues
#234, #236, #237, #238, and #239. It is deliberately separate from the exit
document: an implementation candidate is not a release-qualified result until
the focused tests, platform evidence, and external-parser checks are green on
one exact merged SHA.

Baseline under qualification: `origin/dev` at
`20593d70dfddc633ee9d8644659c6d3828a89ef4`.

Session 10 candidate branch: `cursor/session-10-trust-qualification` (record
exact merged SHA in `docs/SESSION_10_HANDOFF.md` after landing).

| Issue criterion | Implementation location | Test / audit | Windows evidence | Linux evidence | Exact SHA | Status |
| --- | --- | --- | --- | --- | --- | --- |
| #234 canonical reducer; PASS/FAIL/INCOMPLETE/ERROR, waivers, zero-finding budget exhaustion, distinct PdfTool exits | `LoopLibCore/sources/pdfpreflightverdict.h`, `PdfTool/pdftoolpreflight.cpp`, `LoopEditorPlugins/LoopPreflightPlugin/preflightreportmodel.cpp` and report dock | `UnitTestsPreflightVerdict`, `UnitTestsPreflightEngine`, `UnitTestsPreflightPlugin`, `UnitTestsOperatorAcceptance`; direct four-state PdfTool fixture matrix; semantic-trust source audit | Focused targets required on merged SHA | Focused targets required on merged SHA | Session 10 merge SHA | Open — Session 10 does not re-close #234; await merged-SHA CI |
| #236 one artifact/revision authority; complete revision-bound jobs and stale rejection under concurrent mutation | `LoopLibCore/sources/pdfdocumentcontext.*`, `pdfjobscheduler.*`, cache-key types | `UnitTestsIdentitySeparation`, `UnitTestsDocumentSession`, `UnitTestsJobScheduler`, `UnitTestsRevisionStress`; 64-round concurrent render/preflight/thumbnail/repair-plan stress | Focused targets required on merged SHA | Focused targets required on merged SHA | Session 10 merge SHA | Open — Session 10 does not re-close #236; await merged-SHA CI |
| #237 one durable provenance chain; seven kinds, tamper detection, rollback append, retention, live PdfTool flows | `LoopLibCore/sources/pdfoperationhistory.*`, `pdfoperationhistorystore.*`, `PdfTool/pdftoolpreflight.cpp`, `pdftoolrepair.cpp`, `pdftooladdbleed.cpp` | `UnitTestsOperationHistory`, `UnitTestsLifecycle`, `UnitTestsOperatorAcceptance::livePdfToolFlows_writeVerifiableProvenance`, independent SQLite probe, provenance source audit; `UnitTestsRepairOperatorAcceptance` real-path repair | `repair --operation add-bleed` no longer fails `repair.unexpected-change` for bleed-missing + unicode paths once PdfTool tests run on merged SHA | Same repair regression required on merged SHA | Session 10 merge SHA | **T-01 candidate** — code + regressions landed; await merged-SHA PdfTool proof |
| #238 one scheduler submission boundary; no new unmanaged launches; typed GUI handoff and platform cancellation proof | `LoopLibCore/sources/pdfjobscheduler.*`, `LoopLibCore/sources/pdfdiff.cpp`, `scripts/ci/check_unmanaged_async.py`, CI source-integrity jobs | `UnitTestsJobScheduler`, `UnitTestsWorkloadEnvelope`, `UnitTestsLifecycle`, unmanaged-async source audit | Scheduler cancellation + stale-result tests run in CI on Windows | Same scheduler tests run in CI on Linux | Session 10 merge SHA | **T-02 candidate** — `PDFDiff` migrated; unmanaged-async allowlist empty; await merged-SHA CI |
| #239 explicit save policy; destructive operations cannot append incrementally; source remains immutable; recovered output not approved | `LoopLibCore/sources/pdfsavepolicy.*`, writer policy integration, repair history | `UnitTestsRepairOperation`, `UnitTestsIncrementalSave`, live repair provenance; independent parser/signature validator required | Save-policy/repair tests required on merged SHA | Same tests required on merged SHA | Session 10 merge SHA | Open — independent parser/signature evidence tied to T-03 |

The Session 10 merge SHA is not a release-qualified result while T-03 evidence
remains `incomplete` or any row above lacks merged-SHA platform proof.
