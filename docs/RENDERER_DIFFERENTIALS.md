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
or overprint correctness.
