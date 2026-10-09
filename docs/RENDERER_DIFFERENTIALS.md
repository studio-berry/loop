# Renderer differentials

Internal color and overprint regression checks use the Output Preview /
`PDFTransparencyRenderer` path. This policy selects the Core renderer; it does
not establish independent correctness or standards compliance.

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

## Fidelity and origin on the two preview surfaces (#28)

The ordinary canvas banner and the Production Preview both render the same
projection, `EditorHost::previewFidelityStateName()` /
`previewFidelityOriginName()` / `previewFidelityVisual()` /
`previewFidelitySummary()`, so neither surface presents the fast canvas pixels
as the authoritative render. The state vocabulary is `unavailable`, `stale`,
`exact`, `approximate` and `authoritative`; the origin is `none`, `fast-canvas`
or `output-preview`. `approximate` is the overprint-sensitive page on the fast
canvas path, and `previewRequiresAuthoritative()` is true exactly then: its
`previewFidelitySummary()` states that those pixels cannot stand as proof of
print-safe output, and no preview state resolves to a pass treatment.
`EditorHost::ensureAuthoritativePreview()` is the explicit switch the preview
surfaces call before a page is presented as proof; it moves the current page to
the output-preview render and returns false when the page is already
authoritative or exact.

Fidelity is unavailable until Core admits diagnostics for the current page's
requested render key. Selecting the authoritative path does not establish
rendered evidence: while its pixels are pending, refused, or failed, fidelity
is `unavailable` and origin is `none`. A stale preview retains its stale
treatment. The canvas keeps the fidelity-and-origin message visible for every
open document, including exact fast-canvas pages.

The software Quick interaction smoke can exercise the shipped controls with
both visual fixtures:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
ProductQuickAccessibilitySmoke.exe --preview-fixtures loop-preflight/testdata/fixtures
```

It checks the fast-canvas message, the explicit keyboard switch in Production
Preview, unavailable evidence while the output render is pending, and admitted
authoritative pixels with their output-preview cache identity. It then checks
the authoritative message on the ordinary canvas. The cases are
`overprint-cmyk-mode1-on.pdf` and `transparency-normal-cmyk.pdf`; pixel
differentials remain the committed renderer suites described above.

`UnitTestsPageSurface::fastCanvasOverprintRenderIsNotTheAuthoritativeGolden`
proves the distinction against the same committed baseline: for
`overprint-cmyk-mode1-on.pdf` the marked (authoritative) render matches
`overprint-cmyk-mode1-on.png` while the unmarked fast render does not, and the
fast render's own diagnostics report the approximation rather than exact
overprint fidelity. `UnitTestsEditorHost::previewFidelityNamesTheOriginAndSwitchesExplicitly`
proves the host projection and the explicit switch, and
`UnitTestsLoopStateVisual::previewFidelityNeverClaimsAPass` pins that no
fidelity state reads as a pass. The interactive preview still certifies nothing
about publication safety.

## Independent measurement scope

With `LOOP_INDEPENDENT_RENDER_DIR` set, `UnitTestsOverprintRender` additionally
exports DeviceCMYK process planes and all named spot separations from the
controlled, text-free fixtures in [`independent-claims.json`](independent-claims.json).
These exports do not refresh the existing PNG goldens. The Python qualification
harness measures the planes against Ghostscript's [tiffsep device](https://ghostscript.readthedocs.io/en/latest/Devices.html#tiff-file-formats),
retaining both outputs, raw commands, tool/profile identities, delta images and
numeric region results. Missing or extra separations fail coverage.

Settings are fixed: 128 by 128 pixels fitted to the page, 72 dpi Ghostscript
geometry, 8-bit planes, no text or substituted fonts, Generic Core CMS and
Ghostscript fast DeviceCMYK conversion with pinned default ICC profiles. Each
fixture specifies its measurement regions and tolerances before execution.
The flattened case also exports its final PDF at 72 dpi for independent rendering.
A mismatch fails qualification. Never tune regions or tolerances, regenerate a
sealed output, or refresh a regression golden to suppress an independent mismatch.

Claims cover only the recorded fixture planes and settings. They do not prove
calibrated ICC colorimetry, arbitrary fonts, press behavior, or general PDF/X or
PDF/A conformance. Ordinary canvas rendering and same-engine goldens cannot
supply those claims. Render-dependent inspection receipts disclose this limit
through their existing limitations field.

The initial Windows Ghostscript 10.08.0 diagnostic measured all ten cases. Seven failed separation coverage and three exceeded the fixed pixel tolerances. Full commands, planes and differences are retained in the local reproduction artifacts described in [the quality-review handoff](INDEPENDENT_QUALIFICATION_REVIEW.md). These results establish no rendering-fidelity qualification; the gate remains closed.
