# Preflight workflow — Stage 03: governed fix path

Scope: one Core-owned **validate → plan → confirm → execute → verify → publish** boundary for repair operations and Action Lists. This stage does not implement Stage 04's finding-delta or impact-driven recheck (#129/#267) and does not issue certificates (Stage 05).

## Repair validator truth (#626)

`PDFRepairPlan::validators` is not evidence of execution. The existing `runDeclaredRepairValidators` is the single validation runner. This stage makes it:
- produce a validation record for **every declared validator**, never only `NormalPreflight`;
- reopen the isolated candidate for structural proof, then run one effective preflight for normal and check-specific proof;
- treat a required check absent from the effective postflight as **incomplete** (not as a pass obtained from another check);
- reject unsupported `Custom`, text-extraction and signature validators until they have executable contracts;
- reject missing or invalid profiles, cancelled/incomplete postflights and failed checks, preserving the failed/incomplete reason;
- return the inspection evidence to consumers rather than independently rerunning a second CLI validator.

`PDFRepairTransaction::apply()` produces an isolated **Applied** candidate, not a validated/publishable result. Its `validateCandidate()` validates the full profile on that candidate and sets **Passed** only after all declared validators complete. Failed or incomplete validation discards the transaction candidate and leaves source bytes unchanged. Previews remain available *before* validation; they are not publication.

`PdfTool repair` runs the transaction validator gate after previews and before history/approval/output publication. Its repair result contains the exact attempted validation records and verdict; a missing profile or an incomplete preview still blocks publication when `--allow-incomplete` is present. `--allow-incomplete` can request an incomplete result for review, never a successful output write.

`standards-convert` already invokes its independent standards validator within its operation implementation. The unexecutable generic `Custom` validator declaration is removed; no generic runner is claimed for that operation. Explicit `OutputIntent` still requires an actually executed output-intent check in the selected effective preflight profile.

## Action Lists and confirmation

A governed Action List step invokes the shared declared-validator runner even when its profile is missing, recording incomplete evidence and a failed step rather than a validated success. Explicit in-memory `requirePostflight=false` execution remains supported for ungoverned experiments; it carries a warning that declared validators were not run, and it is not a publication contract.

The Editor already exposes separate validate, plan and confirm methods. Stage 03 binds its confirmation identity to **recipe bindings, selected preflight profile bytes and preflight variable bindings**. A selected-profile change, variable change or profile-file reload invalidates a confirmed plan. After the worker finishes, the controller also compares the resulting plan digest against the exact digest the operator confirmed. A mismatch produces Failed and does not apply the candidate. The CLI's explicit command invocation remains governed by its existing approval policy and source/plan/candidate digest gate.

## Action List v1 migration (#637)

The `dev` branch already has a deterministic Action List v1→v2 migrator in `pdfschemaversion.cpp`; do not create a duplicate. Its prior golden contained no usable recipe. Stage 03 replaces that golden with a valid v1 recipe and proves v2 parsing, preserved step semantics, repeatable migration and unsupported-major refusal. Treat #637 as an independent compatibility-contract issue until its full acceptance checks are qualified.

## Acceptance and limitations

Run the owning `UnitTestsRepairOperation`, `UnitTestsRepairOperatorAcceptance`, `UnitTestsActionList`, `UnitTestsSchemaEvolution`, `UnitTestsGovernedExecution`, impacted CLI/Editor acceptance and `python scripts/agent/check-change.py --base origin/dev`. Platform CI must qualify the branch; documentation does not represent unrun tests as passed.

PR #645 overlaps the shared revalidation path and may advance the report schema. Resolve and rerun the combined validation suite before merging Stage 03; do not copy its finding-delta implementation here.
