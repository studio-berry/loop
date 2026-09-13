# Session 10 — Close trust and independent-validation gates

## Scope

Session 10 closes Issues 31–33 on one exact candidate SHA:

- **T-01:** `repair --operation add-bleed` trust contract on real-path fixtures
- **T-02:** Governed async boundary with terminal cancellation and stale-result proofs
- **T-03:** Independent external validation evidence on Linux and Windows

Baseline: `origin/dev` @ `6a55130c…` (Session 09 #535 + Session 13 #539 + baseline record #540).

## Implementation

### Issue 31 — trust contract repair and async semantics

- **add-bleed diff classification:** `PDFAddBleedRepair` now declares
  `expectedChanges.images` so resource-level bleed artwork edits classify as
  expected instead of triggering `repair.unexpected-change`.
- **Real-path repair regression:** `UnitTestsRepairOperatorAcceptance` drives
  `repair --operation add-bleed` through unicode and space path segments
  (`shop files/café poster.pdf`).
- **Governed async:** `PDFDiff` asynchronous comparison submits through
  `PDFJobScheduler` instead of `QtConcurrent::run`; the unmanaged-async audit
  allowlist is now empty.

### Issue 32 — independent external evidence

- Frozen conversion/qualification manifest:
  `docs/evidence/session-10-trust/conversion-fixture-manifest.json`
- Builder:
  `scripts/qualification/build_session10_trust_evidence.py`
- Evidence slots (schema-conformant, refreshed on merge SHA):
  - `docs/evidence/session-10-trust/independent-validation-windows.json`
  - `docs/evidence/session-10-trust/independent-validation-linux.json`

### Issue 33 — qualify trust gates

- Closeout matrix T-01–T-03 rows updated in `docs/0.2.0-closeout-matrix.md`
- Acceptance ledger refreshed in `docs/SEMANTIC_TRUST_ENGINE_ACCEPTANCE.md`

## Verification record

Run after merge records the authoritative `candidate_sha`.

| Check | Command / target | Result |
| --- | --- | --- |
| Unmanaged async audit | `python scripts/ci/check_unmanaged_async.py` | Required green |
| Trust contract sources | `python scripts/ci/check_trust_contract_sources.py` | Required green |
| Repair operator acceptance | `UnitTestsRepairOperatorAcceptance` | Requires PdfTool build |
| Repair operation diff | `UnitTestsRepairOperation::addBleedExpectedChanges_areMeasuredWithoutUnexpectedDiff` | Requires build |
| Job scheduler cancellation | `UnitTestsJobScheduler` + `UnitTestsLifecycle` | Requires build; same tests run on Linux and Windows CI |
| Independent validation builder | `python scripts/qualification/build_session10_trust_evidence.py` | Writes platform evidence; exit 0 only when validators pass |
| Agent proof | `python scripts/agent/check-change.py --base origin/dev` | Required green |

## Gate disposition

| Gate | Local disposition | Hosted follow-up |
| --- | --- | --- |
| T-01 | Code + regression tests landed | Re-run `UnitTestsRepairOperatorAcceptance` on merged SHA in CI |
| T-02 | Scheduler migration + existing cancellation/stale tests | Confirm Linux + Windows CI green on merged SHA |
| T-03 | Manifest + evidence schema frozen | **Blocked** until hosted runners install `qpdf`, `pdfsig`, and `verapdf` and regenerate both evidence JSON files with `status: passed` |

## Remaining blockers

1. **Hosted validators (T-03):** Local qualification host lacks `qpdf`, `pdfsig`,
   and `verapdf`. Evidence files are intentionally `incomplete` until a hosted
   Linux and Windows run executes
   `python scripts/qualification/build_session10_trust_evidence.py` on the exact
   merged candidate SHA.
2. **PdfTool integration tests (T-01):** Require a configured build of PdfTool
   and focused test targets; not run in this workspace session without configure.
3. **Notion reconciliation (Issue 33):** Manual update of Session 10 / Issues
   31–33 pages after merge.
