# Preflight workflow suite — staged implementation contract

Status: proposed execution plan. Baseline: `dev` at `8c7decbcec8034ca52e505bcf39554f47a0433c9` (2026-09-18). Re-verify refs and issues before each stage; this document does not assert that unmerged PRs are complete.

## Goal

Finish the operator path **detect → pinpoint → inspect → plan/confirm/fix → recheck → sign off** in one Core-controlled evidence and revision model. See #134. The Editor renders Core's findings and verdict, PdfTool and PageMaster consume the same contracts, and any unsupported/incomplete check or unverified repair fails closed. Do not introduce a parallel GUI preflight engine, repair writer, provenance ledger, or certificate verifier.

## PR boundaries and sequence

One reviewable PR per implementation stage. Base new topic branches on current `dev`, unless a stage explicitly depends on an unmerged predecessor, in which case document the temporary stacked base and retarget to `dev` after the predecessor merges. Never merge both alternative implementations of the same issue.

| Stage / PR | Scope and owner | Acceptance gate |
| --- | --- | --- |
| **00: Untrusted Action List boundary** (this PR, #636) | Reject malformed `steps`, `params`, `when`, and `when.previousStepStatus` before constructing trusted recipe state. Preserve the caller's result on failure. | Negative Qt tests for wrong types, null, absent required params; valid recipes still round-trip. The existing schema stays unchanged. |
| **01: Detect + effective scope** (#125; existing #124/#130/#131 closed) | Profile/check restrictions for pages, page boxes, anchored regions, layers and object classes. Intersect profile, check, and CLI scopes by narrowing only. Reuse #132 profile identity/provenance (PR #621 merged). | Full-scope vs scoped fixture matrix; unsupported dimensions are `not_inspected`, empty scopes `not_applicable`; neither silently yields a clean pass; Core/PdfTool/Editor scope parity. No new raw-detection implementations unless a closed check fails qualification. |
| **02: Pinpoint + inspect integration** (#127/#195/#196 closed, verify acceptance rather than rewrite) | Validate stable finding IDs, evidence-to-object correlation, honest targeting capability, overlays, inspector fields, and selected-finding state across mutation. | Operator acceptance test: selecting a finding selects the correct page/object or explicitly reports unsupported targeting; stale finding cannot navigate as current. If existing tests already prove this, limit the PR to missing tests/integration defects. |
| **03: Governed fix path** (#626; related #30/#266/#588) | One validate → plan → confirm → execute → publish boundary shared by Action Lists, CLI and Editor. Execute declared repair validators rather than treating JSON metadata as proof. Establish explicit schema-migration disposition for #637 in its own contract decision before claiming old recipes supported. | Missing/invalid confirmation, stale source or plan, unrun validator, cancellation, or denied publication cannot emit a successful repair; unaffected source stays unchanged. Never silently coerce recipe input. |
| **04: Recheck and impact** (#129/#267) | Consolidate **#642 and #644** as alternatives for #267, then integrate #645 for #129. Run revalidation against the serialized candidate; derive deterministic resolved/unchanged/introduced/incomplete finding sets, preserve unaffected evidence, and fall back to full validation whenever impact or baseline is uncertain. | Golden fixture targeted/full verdict parity, new findings and incomplete inspections block success, revision binding and PageMaster parity. Retain one impact planner and one finding-delta implementation; resolve branch overlap before merge. |
| **05: Decision + certificate** (#126 closed; #133 in PR #643; #237) | Decisions remain separate from raw finding severity; certified preflight binds exact document bytes, effective profile, evidence report, approvals, and the canonical operation-history chain. | Tampered chain, changed document/profile, stale waiver, skipped check, missing audit commit, and incomplete run are rejected; warning-only or properly waived cases follow the documented certificate policy. Do not represent tamper-evident attribution as a digital signature. |
| **06: Complete workflow acceptance** (#134; optional portable export #589 is separate) | Exercise UI and CLI through detect → select → inspect → plan → confirm → execute → re-preflight → decision → certificate/verify, including PageMaster where applicable. | End-to-end fixture comparison across surfaces, restart/reopen, cancellation, stale-state and hostile-input tests; published artifact and certificate digests match the bytes checked. Close #134 only on recorded CI and operator-acceptance evidence. |

## Merge and conflict constraints

- `dev` is integration; `stable` is release/default. Never implement from `stable` simply because it is GitHub's default.
- Profile provenance #132 has already landed through #621 but its issue remains open; verify acceptance and reconcile issue bookkeeping rather than reimplementing it.
- #642 and #644 both implement #267 on separate heads. Choose the retained implementation after comparing file-level differences and required proof; port unique tests/fixes and close the superseded PR. #645 may overlap their revalidation path.
- #643 implements certification on an independent head. Rebase and retest it after the canonical recheck/fix path; do not issue certificates from a superseded result model.
- Keep the work limited to 0.3.0 workflow completion. Job-intent comparison #591 and later automation are separate subsequent scope.

## Architecture/adversarial review gates

For every stage inspect current code, authoritative contracts, open PRs, and regression tests before writing. Challenge five failures: **false pass** when evidence is missing; **stale state** after mutation; **wrong target** when navigating or selecting; **unverified publication** after a fix; **misleading certificate** after revision/profile/decision change. Give each a failing fixture or explicit reason it is inapplicable. Reject an implementation that adds a second source of truth.

Verification per PR: focused owning Qt target, any touched schema/contract tests, `python scripts/agent/check-change.py --base origin/dev` (or exact merge base), one required branch-named `changes/` fragment, and final anti-slop review. CI is required for platform/build qualification; do not mark an unrun test as passed.
