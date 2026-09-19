# Preflight workflow — Stage 04: recheck and finding delta

Stage 04 extends the existing repair transaction's serialized/reopened-candidate postflight. It uses the single `computeFindingDelta` structure and the operation-owned `PDFOperationImpact` planner; it does not introduce a second preflight or repair engine.

## Verdict boundary

A post-fix delta compares before/after stable finding IDs, including the check and page actually covered in the after run. A defect on page 2 is **not resolved** when only page 1 was rechecked. Missing check-status rows and budget-exceeded checks cannot prove a fix. A targeted check omitted by plan remains `unchanged` and is identified in `carried_forward`, not `resolved`.

`finding_delta.compared` distinguishes a successful comparison of two reports with no findings from an unrun comparison with four default empty arrays. It is not an independent preflight verdict. The normal postflight verdict and the explicit introduced/incomplete delta gates still decide whether repair output is publishable.

The after report's `coverage_scope.revalidation` names the selected checks and zero-based page scope. A scoped step-level recheck alone does **not** prove a whole-document clean result: a terminal full-profile postflight is still required to publish an Action List or a governed repair. Skipped checks and their carried-forward findings cannot be represented as newly cleared.

## Impact-planning invariants

`PDFOperationImpact` is the authoritative semantic scope. Incomplete impact, document-wide impact, a full rewrite, a required independent oracle, an unspecified domain, or an unmapped enabled check conservatively selects full revalidation. A repair plan's page target **cannot narrow** one of those declarations. Full plans clear their page selector so the report does not imply a narrow full run.

For a legitimately targeted impact, repair-plan pages widen (union) the declared affected pages. Any document-scoped target removes page narrowing. Only `image-resolution` and `thin-strokes` are currently treated as page-local in the step planner; other selected checks run across document pages. This conservative allowlist prevents a document-global ink catalog or page-spanning runner from receiving a false page-local PASS.

## Qualification

- `UnitTestsRepairOperation`: deterministic before/after delta; incomplete or omitted check; omitted page of the same check; missing status; compared-empty versus not-run; carried-forward identifiers.
- `UnitTestsOperationImpact`: incomplete, document-wide, full-rewrite and independent-oracle fallbacks; empty page scope on full plans; existing targeted/full image-profile comparisons.
- `UnitTestsRepairOperatorAcceptance`: introduced findings do not publish an output artifact; source hash remains unchanged; resolved fix report carries the delta.
- `UnitTestsActionList`: step-level before/after data and terminal full postflight.

Remaining issue #267 proof: the current after report records selected checks/pages, but does not persist a complete evidence-cache reused/recomputed ledger. The cache invalidation and outcome equivalence criterion needs a separate corpus-backed qualification before #267 can close. Do not label an omitted check's evidence as verified or copied into the candidate result merely because its finding ID is carried forward. Stage 01's profile restrictions and Stage 03's declared-validator changes are separate draft integrations; reconcile the overlapping `pdfpreflightverdict` contract before merging.
