# Operation impact and revalidation

`PDFOperationImpact` (`pdfoperationimpact.h`) is the shared model for evidence
invalidation, post-operation check selection, and evidence reuse. A registered
operation must explicitly declare its impact; the conservative default is
undeclared and forces a full revalidation.

| Field | Meaning |
| --- | --- |
| `domains` | Evidence Graph families whose observations may have changed (images, colorants, strokes, overprint/transparency, fonts) |
| `pages` | Optional 1-based affected pages; empty means all pages when `allPages` is true |
| `objectIds` | Optional semantic targets for auditing and future object-scoped invalidation |
| `declared` | True only when the operation deliberately supplies this contract |
| `allPages` | The operation affects the declared domains on every page |
| `documentWide` | The operation affects document-level constructs and therefore requires full revalidation |
| `fullRewrite` | Output bytes are a rewritten artifact; evidence is not reused |
| `impactComplete` | False when the operation cannot name all of its effects |
| `requiresIndependentOracle` | Always full revalidation plus the operation's independent validator |
| `mutatesDocument` | False only for operations that provably do not change the PDF |

Unknown, undeclared, or incomplete impact selects every enabled check. Full
rewrites, document-wide effects, standards/document policy, and checks that are
not mapped to an Evidence Graph domain also force a full plan. A complete
non-mutating operation reuses the baseline.

`planRevalidation(impact, enabledCheckIds, hasDocumentPolicy)` is the single
decision point for both check selection and evidence invalidation. A targeted
plan records the invalidated domains/pages and authorizes prior evidence reuse
through `invalidatedEvidenceDomains`, `reusableEvidenceDomains`, `checkIds`,
and `reusedCheckIds`. A full plan invalidates every evidence domain and
authorizes no reuse.

`PreflightEngine::revalidate()` turns a targeted run into a complete result.
The baseline overload merges unaffected findings and statuses with newly
recomputed checks when a complete prior report is available. The evidence-graph
overload reruns the selected checks, drops prior evidence in the invalidated
scope, and carries forward only findings and evidence that the plan proves
unaffected. If the prior inspection or prior evidence was incomplete, it fails
closed to a full run. Calling `run()` on a plan that would reuse checks without
a baseline is deliberately `INCOMPLETE` with `revalidation-baseline-required`.
Final reports expose `revalidation` provenance with reused and recomputed
check/evidence IDs.

PageMaster retains its initial preflight result and Evidence Graph for this
reconciliation. Mutation paths that do not yet expose a semantic impact append
an undeclared impact, deliberately forcing full revalidation rather than
guessing.

Profiles containing PDF/X policy are forced to full local revalidation.
`standards-convert` declares `requiresIndependentOracle` and
`impactComplete = false`. Its conversion implementation still requires the
independent validator; a narrow local impact can never bypass that oracle.
