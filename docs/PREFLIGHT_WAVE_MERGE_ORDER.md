# Preflight wave merge order

Loop 0.1.1 semantic-trust work spans `cursor/s00-m0-plan-0158` and wave branches
`wave-a` through `wave-d`. Merge or rebase in this order so later waves do not
reintroduce stale fixes from an older base.

## Required sequence

1. **`dev`** — integration branch; `#286` Sentry upload uses two-level
   `Split-Path` in `scripts/ci/upload_sentry_debug_files.ps1`. Do **not** merge
   `ci/sentry-debug-files` without rebasing (single-level path bug).
2. **`cursor/s00-m0-plan-0158`** — scheduler preflight in PdfTool, provenance
   kinds (`PreflightRun`), repair history improvements. Rebase onto `dev` before
   wave merges.
3. **`cursor/wave-a-substrate-0158`** — shared substrate; rebase onto post-S00 `dev`.
4. **`cursor/wave-b-evidence-graph-0158`** — evidence graph / color-mode geometry
   (`#310`).
5. **`cursor/wave-c-profiles-impact-0158`** — profile identity / digest (`#311`).
6. **`cursor/wave-d-hostile-load-0158`** — scheduler sidecar bounding (`#309`);
   must include S00 provenance kinds before merge.

## Fix priority (before or during merge)

| Order | Issue | Topic |
| --- | --- | --- |
| 1 | #286 | Sentry `repoRoot` path |
| 2 | #303 | Preflight scheduler timeout race |
| 3 | #310 | Evidence color classification + image bbox |
| 4 | #311 | Profile export digest round-trip |
| 5 | #309 | Sidecar stdout/stderr cap while process alive |
| 6 | #303 | Terminal provenance on repair verification failures |

## Verification after each merge

Run targeted tests (not necessarily full `ctest`):

- `UnitTests` — `JobScheduler`, `PreflightEngine`, `PreflightProfileIdentity`,
  `PreflightPlugin`, `OperationHistory`
- `PdfTool preflight` smoke on a generated fixture

## Branch divergence note

S00 and wave branches diverged after `1b7b1027`. S00 carries provenance commits
wave-d lacks; wave-d carries hostile-load scheduler-plugin work S00 lacks. Always
rebase the next wave onto the updated `dev` tip, never merge wave-d before S00
without reconciling `PreflightRun` history and plugin scheduler paths.
