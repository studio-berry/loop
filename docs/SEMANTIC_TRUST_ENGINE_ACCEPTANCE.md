# Semantic trust engine — grouped Session 1 acceptance matrix

This is the qualification ledger for the grouped Session 1 pass over issues
#234, #236, #237, #238, and #239. It is deliberately separate from the exit
document: an implementation candidate is not a release-qualified result until
the focused tests, platform evidence, and external-parser checks are green on
one exact merged SHA.

Baseline under qualification: `origin/dev` at
`20593d70dfddc633ee9d8644659c6d3828a89ef4`.

Implementation evidence commit: `7912493e234f1abad3b90f3b813616aeb9d1fd63`
(topic branch `gh-234`; not merged or tagged).

| Issue criterion | Implementation location | Test / audit | Windows evidence | Linux evidence | Exact SHA | Status |
| --- | --- | --- | --- | --- | --- | --- |
| #234 canonical reducer; PASS/FAIL/INCOMPLETE/ERROR, waivers, zero-finding budget exhaustion, distinct PdfTool exits | `LoupeLibCore/sources/pdfpreflightverdict.h`, `PdfTool/pdftoolpreflight.cpp`, `LoupeEditorPlugins/LoupePreflightPlugin/preflightreportmodel.cpp` and report dock | `UnitTestsPreflightVerdict`, `UnitTestsPreflightEngine`, `UnitTestsPreflightPlugin`, `UnitTestsOperatorAcceptance`; direct four-state PdfTool fixture matrix; semantic-trust source audit | 15/15 focused targets green; direct exits/states: pass 0, fail 1, incomplete 8, error 9; waiver/budget cases green | Not run in this session | `7912493e234f1abad3b90f3b813616aeb9d1fd63` | Open — Windows candidate evidence green; Linux and merged-SHA evidence open |
| #236 one artifact/revision authority; complete revision-bound jobs and stale rejection under concurrent mutation | `LoupeLibCore/sources/pdfdocumentcontext.*`, `pdfjobscheduler.*`, cache-key types | `UnitTestsIdentitySeparation`, `UnitTestsDocumentSession`, `UnitTestsJobScheduler`; 32-round render/preflight/thumbnail/repair-plan stress | 15/15 focused targets green, including 32-round concurrent stale-result rejection | Not run in this session | `7912493e234f1abad3b90f3b813616aeb9d1fd63` | Open — Windows candidate evidence green; Linux and merged-SHA evidence open |
| #237 one durable provenance chain; seven kinds, tamper detection, rollback append, retention, live PdfTool flows | `LoupeLibCore/sources/pdfoperationhistory.*`, `pdfoperationhistorystore.*`, `PdfTool/pdftoolpreflight.cpp`, `pdftoolrepair.cpp`, `pdftooladdbleed.cpp` | `UnitTestsOperationHistory`, `UnitTestsLifecycle`, `UnitTestsOperatorAcceptance::livePdfToolFlows_writeVerifiableProvenance`, independent SQLite probe, provenance source audit | 15/15 focused targets green; live preflight/add-bleed sidecars contain revision/profile/output digests and terminal status; SQLite integrity probe green | Not run in this session | `7912493e234f1abad3b90f3b813616aeb9d1fd63` | Open — generic `repair --operation add-bleed` still returned `repair.unexpected-change`; Linux and merged-SHA evidence open |
| #238 one scheduler submission boundary; no new unmanaged launches; typed GUI handoff and platform cancellation proof | `LoupeLibCore/sources/pdfjobscheduler.*`, `scripts/ci/check_unmanaged_async.py`, CI source-integrity jobs | `UnitTestsJobScheduler`, `UnitTestsWorkloadEnvelope`, unmanaged-async source audit | Scheduler/workload tests and source audit green; audit reports 13 known legacy product `QtConcurrent::run` call sites | Not run in this session | `7912493e234f1abad3b90f3b813616aeb9d1fd63` | Blocked — product-facing unmanaged launches remain and Windows/Linux cancellation proof is not complete |
| #239 explicit save policy; destructive operations cannot append incrementally; source remains immutable; recovered output not approved | `LoupeLibCore/sources/pdfsavepolicy.*`, writer policy integration, repair history | `UnitTestsRepairOperation`, `UnitTestsIncrementalSave`, live repair provenance; independent parser/signature validator required | Save-policy/repair tests green; signed annotation/metadata fixture preserves original signed prefix; no independent PDF parser/signature validator available | Not run in this session | `7912493e234f1abad3b90f3b813616aeb9d1fd63` | Blocked — independent parser/signature evidence absent |

The implementation SHA above is a topic-branch candidate, not an exact merged
release SHA. The semantic-trust exit document must not be marked green while
any row above is open or blocked.
