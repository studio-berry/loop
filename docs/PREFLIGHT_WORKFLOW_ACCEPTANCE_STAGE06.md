# Preflight workflow — Stage 06: complete workflow acceptance

Status: acceptance evidence for #134. Baseline: `dev` at `5fdfee1c` (review-batch integration plus the `unstable` sync).

Stage 06 proves the operator path **detect → pinpoint → inspect → plan/confirm/fix → recheck → sign off** end to end instead of adding another engine. `UnitTestsPreflightWorkflowAcceptance` drives the published CLI through the whole chain on a real fixture and then exercises the fail-closed certificate paths; every other criterion below is owned by an existing target, and that mapping — plus the recorded CI run — is the evidence the issue asks for.

## Operator path, one fixture, one run

`UnitTestsPreflightWorkflowAcceptance::workflow_detectFixRecheckSignOffVerify`

- **Detect** — `preflight` on `bleed-missing.pdf` exits 1 (findings), the report's verdict is `fail` and the `bleed` check is named in the findings.
- **Fix + recheck** — `repair --operation add-bleed` exits 0, the report is `passed`, `postflight.pass` is true, and `finding_delta` resolves the bleed finding with empty `introduced` and `incomplete` sets. The trusted source is byte-identical afterwards.
- **Sign off** — `preflight --certify` issues a certificate whose `document_revision_digest` equals the published artifact's independently recomputed SHA-256, with a populated `effective_profile_digest`, `report_digest` and `audit_chain_head_event_id`.
- **Certificate/verify** — a separate `verify-certificate` process re-reads the artifact, the certificate and the audit sidecar and returns `valid`. Because that process shares nothing with the issuing run but the files, it is also the restart/reopen case.

## Fail-closed certificate paths

Same target, one case each:

| Case | Expected |
| --- | --- |
| Artifact bytes changed after certification | `invalid-document-changed`, exit 1 |
| Certificate presented with an uncertified document | `invalid-document-changed`, exit 1 |
| Same bytes copied without the audit sidecar | `invalid-audit-chain-broken`, exit 1 |
| Certification requested for a failing report | refused, and no certificate file is written |
| A fix supersedes the certified revision | `CertificateInvalidated` recorded in the certified document's own chain, later verification returns `invalid-certificate` |

## Surfaces

| Criterion | Owner |
| --- | --- |
| CLI chain, certificate/verify, digests | `UnitTestsPreflightWorkflowAcceptance`, `UnitTestsPdfToolContract` |
| Editor: select → inspect → plan → confirm → execute | `UnitTestsEditorHost` (`exportedPreflightReportMatchesPdfToolForTheSameInputs`, `restrictedPreflightReportMatchesPdfTool`, `preflightRunsOffInteractiveThread`), `UnitTestsShellInspectorDispatch` (`staleFindingCannotSelectInspectorOrCanvas`), `UnitTestsFindingNavigation`, `UnitTestsProductOperatorLoop` |
| PageMaster (preflight gate + revalidation) | `UnitTestsPageMasterExport` (`preflight_gate_blocksFailedOutput`, `preflightGate_enablesRevalidationByDefault`, `preflight_sidecarWriteFailure_failsClosed`, `resume_preflightProfileIdentityDriftRejectsResume`) |
| Cancellation | `ActionListTest::cancellationLeavesSourceUntouched`, `PreflightInteractionTest::controllerMarksAnInFlightRunStaleAndCancelsIt`, `OperationHistoryTest::cancelledPreflightRunIsNotAccepted`, `PageMasterExportTest::cancel_midOutput_beforeWrite_writesNothing`, `SafeFileWriterTest::writeDevice_cancelledProducer_leavesOriginalUntouched` |
| Stale state | `ActionListTest::selectExecuteFailsClosedWhenRevisionDigestStaleAtExecute`, `PreflightInteractionTest::staleRetainedReportCannotBeRestoredByFailedOrCancelledRerun`, `FindingNavigationTest::staleRequestsClearPresentationAndCannotBecomeCurrent`, `ShellInspectorDispatchTest::staleFindingCannotSelectInspectorOrCanvas`, `GovernedExecutionTest::publishRequiresMatchingPlanBoundApproval` |
| Hostile input | `ActionListTest::rejectsMalformedStepInput`, `ActionListTest::publishGateRejectsMalformedProfile`, `RepairOperationTest::declaredValidators_rejectMalformedProfileBeforePublish`, `EditorHostTest::importDigestMismatchProfileIsRejected` |

Portable export (#589) remains separate scope.

## Defects found by this acceptance, and fixed

1. **The audit chain could not be reproduced by any reader.** `appendEvent` hashed the output artifact identity the caller passed — a role-specific label such as `preflight-input.pdf` — while a reader reconstructs that identity from the `artifacts` row, which keeps the first registered name (`candidate-output.pdf`). Every chain that contained an audited artifact therefore verified as *compromised*, and certification and verification refused. Fixed in `PDFOperationHistoryStore::appendEvent`: the artifact is registered and the **persisted** identity is what gets hashed, so the hash covers exactly what a read can rebuild. Immutable-metadata conflicts still fail loudly through `registerArtifact`.
2. **Certification digested a report the chain never stores.** The report digest compared the unredacted in-memory report with the redacted report recorded in the event, so issuance always failed with *"Certification requires an accepted preflight event in the audit chain."* Fixed in `issuePreflightCertificate`: the expected digest is computed over the persisted (redacted) form, which is the only form a verifier can re-derive.
3. **A superseded certificate was never invalidated.** The repair CLI read the retained certificate from, and wrote its invalidation to, the *output* document's history — which never holds the certificate being superseded — so `verify-certificate` kept returning `valid` after a fix changed a certified revision. Fixed in `PdfTool/pdftoolrepair.cpp`: the retained certificate is read from, and the invalidation recorded in, the certified document's own chain, with its own execution record in that chain.

Histories written before fix 1 keep their compromised chain, because the stored hashes are immutable; re-running the workflow produces a chain that certifies and verifies. No migration rewrites audit history.
