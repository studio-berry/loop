# Preflight restriction behavior (Stage 01)

This document records what the current Core engine can **actually inspect** under
profile/check/CLI restrictions. The JSON schema describes requested scope; it is
not a promise that every check has an implementation for every dimension.

## Contract

1. Profile scope applies to every enabled check. Check scope is an intersection,
   never an override that widens profile scope. CLI `--page-first`,
   `--page-last`, and `--page-select` intersect with both.
2. Pages are 1-based inclusive in authored profiles. In normalized reports,
   `scope_restrictions.pages` is a sorted list of 1-based page numbers.
   An empty effective set cannot be a successful preflight.
3. Scope is recorded on each check status, each produced finding, and the report's
   `coverage_scope`. A CLI page restriction also enters the effective-profile
   digest; it never changes the authored profile file digest.
4. A check which cannot honor a requested dimension reports
   `not_inspected / restriction_unsupported:<dimension>` and makes the whole
   run incomplete, rather than silently inspecting the whole document.
   An empty page selection reports `not_applicable /
   restriction_excluded_all_content`; it is not a clean PASS.
5. For `ink-coverage`, a supported `restrictions.page_box` selects the actual
   raster probe analysis box instead of merely annotating the result.

## Current capability matrix

| Dimension | Honored by | Explicitly uninspected by |
| --- | --- | --- |
| `pages` | Evidence Graph-backed checks and `ink-coverage` | Non-graph, page-spanning check runners (e.g. `bleed`, `output-intent`) until they take a per-page selector |
| `page_box` | `ink-coverage` with media/crop/trim/bleed; `image-resolution` and `thin-strokes` with geometric evidence (including art) | Other checks; `art` for ink coverage |
| `regions` | Include-only anchored regions on `image-resolution` and `thin-strokes`; includes intersect so check scope never widens profile scope | Other checks and exclude-mode regions; unresolved target or anchor geometry is `not_inspected` |
| `layers` | None yet | All checks until collected evidence contains verified OCG membership |
| `object_classes` | `image-resolution` uses `image`; `thin-strokes` uses `vector` | Other checks until their evidence has a trustworthy object-class mapping |

The uninspected cells are residual implementation work for #125, not a claim of
support. Fixes must not be offered as safe on an incomplete scoped run.

## CLI examples

```sh
PdfTool preflight artwork.pdf --profile press.json --page-first 3 --page-last 8 --console-format json
PdfTool preflight artwork.pdf --profile press.json --page-select '1,5,7-11' --console-format json
```

`--page-select` also accepts open ends (`-29`, `43-`), bounded by the
document page count. When combined, the three CLI selectors intersect. The CLI
cannot restore a page already excluded by the authored profile or a check.

## Evidence needed before closing #125

- Qt tests for the parser, range limits, resolved scope in findings and check
  statuses, empty intersections, and CLI digest binding.
- PdfTool process-level tests for all three selectors and profile/check narrowing.
- Fixtures showing anchoring against non-origin/non-matching media and trim
  boxes, OCG membership, and object-class targeting. Exclude-mode regions
  remain `not_inspected` until partial object overlap is represented safely.
- Independent Core/Editor/PdfTool parity proof on the same exact PDF revision
  and effective scope.
- Rebase schema changes against #645's report v4 upgrade and rerun its relevant
  corpus and schema-evolution tests before merging Stage 01.
