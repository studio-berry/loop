# S20a / #142 page-surface contract

Status: internal production contract for the Widgets viewport; not a public API.

This document records the smallest contract needed to evaluate cached and
progressive page rendering without creating a presentation-neutral target. The
current Widgets renderer remains the behavior baseline. The test-only probe in
`UnitTestsPageSurfaceProbe` exercises this contract and must not be promoted to
production code without the G2 module/dependency decision.

## Key and identity

Every page surface is identified by the complete `PDFRevisionIdentity`, page
index, rotation, renderer feature set, target pixel size, and device-pixel
ratio. Color-management state is protected by explicit invalidation from the
CMS manager's change signal. The production cache is private to
`PDFDrawWidgetProxy`; profile and renderer changes clear it, while the complete
revision remains part of every key.

A viewport request adds a monotonically increasing generation. The pair is
immutable while surface work is in flight and insertion is rejected when the
generation has been superseded.

The generation advances for every new viewport request, page switch,
cancellation, document revision, cache-generation change, and document
replacement. A result is accepted only when its complete key and generation
still equal the active request. Stale results cannot be inserted, presented, or
uploaded.

## Cache and queue policy

- The existing `setCacheLimit()` byte limit is the sole byte-budget authority
  for both compiled pages and rendered surfaces; no second per-document or
  process-wide policy is added.
- An entry whose checked byte cost exceeds the budget is rejected explicitly.
- Eviction is deterministic least-recently-used by access sequence.
- A compatible surface may be transformed immediately while a newer surface is
  requested. Compatibility requires the same revision, page, rotation,
  render mode, color/output settings, and device-pixel ratio. Zoom bucket and
  target size select the nearest usable surface.
- Interactive/visible work supersedes the previous request for the same active
  surface. The existing prefetch path remains bounded to the current layout's
  one- or two-page look-ahead; the production surface cache itself never adds
  an unbounded render queue.
- No customer-content surface is written to disk.

## Progressive flow

1. Pan or zoom changes the viewport and advances the request generation.
2. The best compatible cached surface is transformed for the next frame.
3. The existing Widgets render path receives the higher-fidelity request and
   renders it through `PDFRasterizer` using the existing compiled page.
4. Completion is checked against revision, generation, page, and key.
5. Only a current result replaces the surface and requests a frame.
6. Overlay-only changes remain an independent pass and do not trigger page
   rendering.

The production trace records compiled-page and rendered-surface cache
hit/miss totals using the existing interaction evidence vocabulary. The probe
retains direct coverage of stale-result, cancellation, byte-budget, eviction,
and priority-shedding policy. The production path does not add a second
scheduler or fold in issue #54's backend partial-page render contract.

## G2 boundary

The current `PDFDrawWidgetProxy` path provides the reusable surface without a
production-neutral seam. The cache remains an internal Widgets implementation;
no neutral library, persistent customer-content storage, or public surface API
is introduced.
