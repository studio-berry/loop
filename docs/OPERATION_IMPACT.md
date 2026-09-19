# Operation impact and revalidation

`PDFOperationImpact` (`pdfoperationimpact.h`) is the single conservative model for
post-operation evidence invalidation, check selection, and evidence reuse.

| Field | Meaning |
| --- | --- |
| `domains` | Evidence Graph families the operation can affect |
| `pages` / `objectIds` | Narrower affected scope when it is known |
| `documentWide` | The effect cannot be safely narrowed below document scope |
| `fullRewrite` | Object identity is not stable across the output artifact; full validation is required |
| `impactComplete` | False when the operation cannot prove its complete semantic effect |
| `requiresIndependentOracle` | Full validation plus the independent standards validator |
| `mutatesDocument` | False only for operations that provably do not change the PDF |

Unknown/incomplete, document-wide, full-rewrite, oracle-required, and unmapped-check
impact always selects a full run. A complete non-mutating operation reuses the
baseline. A complete domain-scoped mutation can select a targeted run.

`planRevalidation(impact, enabledCheckIds)` produces both sides of the same
decision: `invalidatedEvidenceDomains` and `checkIds` describe what must be
recomputed, while `reusableEvidenceDomains` and `reusedCheckIds` describe
what may be carried forward. The engine also records
`recomputedEvidenceDomains` after the profile is known.

Targeted revalidation is valid only with a complete baseline report containing
outcomes for every reused check. `PreflightEngine::revalidate()` merges those
unaffected findings and statuses with newly recomputed checks. Calling a
targeted `run()` without a baseline is deliberately `INCOMPLETE` with
`revalidation-baseline-required`; it cannot become a false clean pass.

Every emitted preflight report includes additive `revalidation` accounting:
full/targeted mode, plan reason, recomputed and reused checks, invalidated,
recomputed, and reusable evidence domains, target pages, and whether a baseline
was actually reused. PageMaster retains its initial complete report and uses it
for post-fix targeted revalidation.

Profiles containing PDF/X policy are forced to full local revalidation.
`standards-convert` additionally declares `requiresIndependentOracle` and
keeps `impactComplete = false`, so a seemingly narrow local impact can never
bypass the external validator.
