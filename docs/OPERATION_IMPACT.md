# Operation impact and revalidation

`PDFOperationImpact` (`pdfoperationimpact.h`) is the shared model for cache
invalidation and post-fix check selection.

| Field | Meaning |
| --- | --- |
| `domains` | `PDFEvidenceDomain` flags from the Evidence Graph (images, colorants, strokes, overprint/transparency, fonts) |
| `pages` / `objectIds` | Optional narrower scope |
| `documentWide` | Affects document-level constructs; plan a full revalidation |
| `fullRewrite` | Output bytes are a new artifact (cache must drop) |
| `impactComplete` | False when the operation cannot name its effects |
| `requiresIndependentOracle` | Always full revalidation plus an external validator |

Unknown or incomplete impact selects every enabled check. Checks that are not
graph-backed (bleed, trim, output-intent, …) also force a full plan, because
they have no Evidence Graph domain to skip.

`planRevalidation(impact, enabledCheckIds)` returns the conservative subset.
Targeted and full verdicts are required to match when the profile only contains
mapped checks for the affected domains.

`standards-convert` always sets `requiresIndependentOracle` and
`impactComplete = false`. Local impact looking narrow does not skip the
independent validator.
