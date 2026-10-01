# Legacy issue provenance

Loop's planning history predates the reset repository. This page records what a bare
issue number means in this tree, where the retired repository's content can still be
recovered, and how references are written from now on.

## What happened

`studio-berry/loop2` was created on 2026-09-24 as the reset codebase and now serves as
`studio-berry/loop`. The repository that held `studio-berry/loop` before the rename is
not reachable from either the `studio-berry` or the `mberrys` account as of 2026-09-28:
`gh repo view` finds no `mberrys/loop`, `mberrys/loop2`, `mberrys/Loop-pdf`, or
`studio-berry/Loop-pdf`, and `studio-berry` lists no repository with that history. Its
issue and pull-request numbers (at least through #686) overlap the reset repository's
sequence, which started again at 1.

Consequences:

- A link to `github.com/studio-berry/loop/issues/<n>` written before the rename either
  returns 404 or opens an unrelated reset issue. Neither is the issue the author meant.
- A bare `#<n>` in a file dated before 2026-09-24 means the legacy issue or pull request
  unless the file says otherwise (`loop2 #15`, `#15`, and every issue in the roadmap's
  L01–L12 suite are reset issues).
- Issue and pull-request numbers share one sequence, so the collision surface grows with
  every new issue or PR.

## Where legacy content is recoverable

The retired repository itself cannot be recovered from GitHub here. The specifications
and dispositions survive in Notion:

- the *Loop Issues* ledger and the *Sessions* ledger, linked from the master roadmap
  (§2) — the source of record for legacy titles, bodies, and status;
- `docs/ROADMAP_0.5.0-0.8.0.md`, `docs/github-milestones/`, and the handoff documents
  under `docs/`, which quote legacy numbers as they were written.

Do not treat a legacy issue's status as reset-repository status. Reconcile against code,
tests, and exact-SHA evidence, and write a new issue for a demonstrated remaining gap.

## How references are written

| Reference | Meaning |
| --- | --- |
| `#<n>` in an issue, PR, or code comment | An issue or PR in `studio-berry/loop` (the reset repository), resolved by GitHub. |
| `legacy #<n>` in prose, `legacy#<n>` in machine-read files | A retired-repository issue. Never linked; never a live tracker. |
| `studio-berry/loop#<n>` or the full URL | A reset issue, when a file could be read outside the repository. |

Rules:

1. **Never link a legacy number.** No `github.com/studio-berry/loop/issues/<n>` URL may
   point at a legacy issue. All 28 legacy issue and pull-request URLs that were under
   `docs/` are rewritten to `legacy #<n>` text. The last three, the links to legacy #656
   and #675 in `docs/GOVERNED_EXECUTION.md` and
   `docs/adr/adr-011-architecture-contracts-d1-d5.md`, waited for a change that carries
   the governed-execution subsystem's binding proof lanes (build, packaging, unit);
   this change carries them, so the rewrite is complete and no exception remains.
2. **Machine-read records name their repository.** `github_issues` entries in
   `docs/preflight-check-catalog-overlay.json` carry `repository`. Live records are
   `#<n>` with `studio-berry/loop`; frozen snapshots are `legacy#<n>` with `legacy`.
3. **A legacy record can document closed work, never open a gap.** The catalog generator
   refuses an open legacy issue as a backlog row's `closed_by`. Re-point the row to a
   live issue.
4. **Live records are read back before promotion.**
   `python3 scripts/generate-architecture-catalogs.py --check --verify-github` compares
   title, state, and milestone with GitHub and rejects a number that now resolves to a
   pull request or another issue.

## Preflight backlog re-pointing

Open backlog rows used to cite legacy trackers. Each now cites the reset issue filed for
the gap; the legacy number is kept here as provenance.

| Backlog row | Legacy tracker | Live tracker |
| --- | --- | --- |
| `barcode-slug-braille`, `dieline-geometry` | legacy #604 | #143 (X00-04) |
| `devicen-per-colorant-ink-limit` | legacy #600 | #142 (X00-03) |
| `gwg-2022-2024-certificates` | legacy #664 | #144 (X00-05) |
| `imposition-and-reader-spreads`, `pdfvt-variable-data` | legacy #603, legacy #605 | #141 (X00-02) |
| `bleed-raster-strip-depth` | legacy #47 | #120 (L01-15) |
| `white-overprint-renderer` | legacy #49 | #119 (L01-14) |
| `transparency-rip-interaction` | unfiled | #119 (L01-14) |
| `color-mode-icc-alternate` | unfiled | #113 (L01-08) |
| `font-glyph-coverage` | unfiled | #114 (L01-09) |
| `hidden-layers-ocmd` | unfiled | #115 (L01-10) |
| `ink-coverage-raster-tac` | unfiled | #116 (L01-11) |
| `invisible-content-breadth` | unfiled | #117 (L01-12) |
| `obscured-content-occlusion`, `off-page-content-clipping` | unfiled | #118 (L01-13) |

Rows that landed (`corrupt-embedded-fonts`, `nested-font-resources`,
`output-intent-identity`, `thin-filled-parts`, `pdfx5-pdfa3-output`) and the closed
`devicen-dieline-detection` row keep their legacy references as `legacy#<n>` snapshots,
as does the `invisible-content-breadth` gap text for its earlier detector.
`color-inventory-probe-depth` and `thin-parts-raster-budget` stay register-only with a
reviewed deferral.

The legacy URL rewrite is complete: `docs/GOVERNED_EXECUTION.md` and
`docs/adr/adr-011-architecture-contracts-d1-d5.md` were the last files holding a legacy
link, and no `docs/` file links a legacy number now. The reset-issue links in
`docs/CORE_QUALIFICATION.md` are live and stay as they are.
