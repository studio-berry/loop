# Renderer differentials

Color and overprint claims are measured against the Output Preview /
`PDFTransparencyRenderer` path, not against page-view overprint.

`UnitTestsOverprintRender` renders each committed fixture at 128×128, compares
pixels to `loupe-preflight/testdata/renders/*.png`, and records numeric
measurements in sibling `*.measurements.json` files:

- image width / height
- max channel delta
- differing-pixel count
- declared budgets (max channel delta 2, 64 differing pixels)

A drift beyond those budgets fails the named test. Refresh goldens only with
`LOUPE_UPDATE_SNAPSHOTS=1`.

**Disclosed limitation:** page-view overprint (the ordinary viewer paint path)
is not this measurement renderer and must not be cited as proof of separation
or overprint correctness. The canvas surfaces this: when the current page's
cached `PDFPrecompiledPage::containsOverprint()` flag is set, it shows a
persistent fidelity indicator and lets the operator escalate that one page to
the authoritative `PDFTransparencyRenderer` path
(`PDFRenderPolicy::forOutputPreview()`), without reopening the document.
`UnitTestsPageSurface::sessionRendererEscalatesToAuthoritativeOverprintMatchingGoldenBaseline`
asserts that escalated render matches `overprint-cmyk-mode1-on.png`, the same
baseline `UnitTestsOverprintRender` checks — so canvas escalation and this
measurement renderer are proven to agree, not just independently plausible.
