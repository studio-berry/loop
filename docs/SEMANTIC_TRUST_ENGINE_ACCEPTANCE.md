# Semantic trust engine — grouped Session 1 acceptance matrix

This is the qualification ledger for the grouped Session 1 pass over issues
#234, #236, #237, #238, and #239. It is deliberately separate from the exit
document: an implementation candidate is not a release-qualified result until
the focused tests, platform evidence, and external-parser checks are green on
one exact merged SHA.

Baseline under qualification: `origin/dev` at
`20593d70dfddc633ee9d8644659c6d3828a89ef4`.

| Issue criterion | Implementation location | Test / audit | Windows evidence | Linux evidence | Exact SHA | Status |
| --- | --- | --- | --- | --- | --- | --- |
| #234 canonical reducer; PASS/FAIL/INCOMPLETE/ERROR, waivers, zero-finding budget exhaustion, distinct PdfTool exits | `LoupeLibCore/sources/pdfpreflightverdict.h`, `PdfTool/pdftoolpreflight.cpp`, `LoupeEditorPlugins/LoupePreflightPlugin/preflightreportmodel.cpp` and report dock | `UnitTestsPreflightVerdict`, `UnitTestsPreflightEngine`, `UnitTestsPreflightPlugin`, `UnitTestsOperatorAcceptance`; PdfTool fixture matrix; semantic-trust source audit | Windows focused suite: 15/15 green; direct pass/fail/error fixtures green; incomplete covered by reducer/engine tests | Not run in this session | Pending topic SHA | Partial — Windows evidence green; Linux and merged-SHA evidence open |
| #236 one artifact/revision authority; complete revision-bound jobs and stale rejection under concurrent mutation | `LoupeLibCore/sources/pdfdocumentcontext.*`, `pdfjobscheduler.*`, cache-key types | `UnitTestsIdentitySeparation`, `UnitTestsDocumentSession`, `UnitTestsJobScheduler`; 32-round render/preflight/thumbnail/repair-plan stress | Windows focused suite: 15/15 green, including 32-round stress | Not run in this session | Pending topic SHA | Partial — Windows evidence green; Linux and merged-SHA evidence open |
| #237 one durable provenance chain; seven kinds, tamper detection, rollback append, retention, live PdfTool flows | `LoupeLibCore/sources/pdfoperationhistory.*`, `pdfoperationhistorystore.*`, `PdfTool/pdftoolpreflight.cpp`, `pdftoolrepair.cpp`, `pdftooladdbleed.cpp` | `UnitTestsOperationHistory`, `UnitTestsLifecycle`, `UnitTestsOperatorAcceptance::livePdfToolFlows_writeVerifiableProvenance`, external SQLite probe, provenance source audit | Windows focused suite: 15/15 green; live preflight/add-bleed sidecars structurally verified; SQLite integrity probe green | Not run in this session | Pending topic SHA | Partial — Windows evidence green; generic repair command still returned unexpected-change; Linux and merged-SHA evidence open |
| #238 one scheduler submission boundary; no new unmanaged launches; typed GUI handoff and platform cancellation proof | `LoupeLibCore/sources/pdfjobscheduler.*`, `scripts/ci/check_unmanaged_async.py`, CI source-integrity jobs | `UnitTestsJobScheduler`, `UnitTestsWorkloadEnvelope`, unmanaged-async source audit | Known legacy call sites remain; no evidence yet | Known legacy call sites remain; no evidence yet | Pending topic SHA | Blocked — 13 product `QtConcurrent::run` call sites remain to migrate |
| #239 explicit save policy; destructive operations cannot append incrementally; source remains immutable; recovered output not approved | `LoupeLibCore/sources/pdfsavepolicy.*`, writer policy integration, repair history | `UnitTestsRepairOperation`, `UnitTestsIncrementalSave`, live repair provenance; independent parser/signature validator required | Independent validator not available in current environment | Independent validator not available in current environment | Pending topic SHA | Blocked — external parser/signature evidence absent |

The exact implementation SHA and per-platform test run identifiers are filled
when this branch is committed and the fresh Windows/Linux builds complete.
The semantic-trust exit document must not be marked green while any row above
is open or blocked.
