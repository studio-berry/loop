# Renderer differentials

Color and overprint claims are measured against the Output Preview /
`PDFTransparencyRenderer` path, not against page-view overprint.

`UnitTestsOverprintRender` renders each committed fixture at 128×128, compares
pixels to `loop-preflight/testdata/renders/*.png`, and records numeric
measurements in sibling `*.measurements.json` files. The substitute-font case
(`font-not-embedded.pdf` → `font-not-embedded.png`) pins the fallback paint path
for non-embedded standard-14 text; preflight `embedded-fonts` detection alone
cannot catch silent substitute-font rendering regressions.

- image width / height
- max channel delta
- differing-pixel count
- declared budgets (max channel delta 2, 64 differing pixels)

A drift beyond those budgets fails the named test. Refresh goldens only with
`LOOP_UPDATE_SNAPSHOTS=1` (Linux is the source of truth; Windows uses the same
PNGs with those budgets):

```bash
LOOP_UPDATE_SNAPSHOTS=1 ctest --test-dir build -R UnitTestsOverprintRender
```

The same target also flattens `transparency-normal-cmyk.pdf` through
`PDFTransparencyFlattener::apply()` at 72 DPI, re-renders the opaque page at
128×128, and compares it to `flatten-transparency-normal-cmyk.png`. The slot
fails closed if flatten reports success but the raster is blank (fewer than
256 non-white pixels) or drifts beyond the shared budgets. Structural flatten
tests in `UnitTestsTransparencyFlattener` only check region reports and dry-run
identity; they cannot catch a silent blank paint.

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
