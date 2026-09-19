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
6. Explicit legacy `analysis_box` is migrated to the effective page box for
   `ink-coverage`; its check status carries `deprecated:analysis_box; use
   restrictions.page_box` as a non-blocking diagnostic. Conflicting old/new
   box requests are invalid rather than silently choosing one.

## Current capability matrix

| Dimension | Honored by | Explicitly uninspected by |
| --- | --- | --- |
| `pages` | Page-local Evidence Graph checks and `ink-coverage`; `color-inventory` only when every document page remains selected | Non-graph, page-spanning runners (e.g. `bleed`, `output-intent`) and partial-page `color-inventory`, whose spot/separation evidence is document-global |
| `page_box` | `ink-coverage` with media/crop/trim/bleed; `image-resolution` and `thin-strokes` with geometric evidence (including art) | Other checks; `art` for ink coverage |
| `regions` | Include and exclude anchored regions on `image-resolution` and `thin-strokes`; included geometry intersects and fully excluded evidence objects are removed | Other checks; unresolved target or anchor geometry is `not_inspected` |
| `layers` | Named OCG membership for `image-resolution` and `thin-strokes` evidence | Other checks; unresolved OCG/OCMD membership is `not_inspected` |
| `object_classes` | `image-resolution` uses `image`; `thin-strokes` uses `vector` | Other checks until their evidence has a trustworthy object-class mapping |

Content overlapping a partially excluded region is still evaluated when it
has a remaining visible portion; no visual clipping is claimed for the finding
bbox. The sampled quantities are object-level image resolution and stroke width,
not region-local pixel measurements.

Uninspected cells are not claims of support. Fixes must not be offered as safe
on an incomplete scoped run.

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
- Fixtures proving offset trim/media anchoring, include/exclude regions,
  named OCG membership, legacy analysis-box migration, and image/vector
  object-class selection.
- Independent Core/Editor/PdfTool parity proof on the same exact PDF revision
  and effective scope.
- Rebase schema changes against #645's report v4 upgrade and rerun its relevant
  corpus and schema-evolution tests before merging Stage 01.
