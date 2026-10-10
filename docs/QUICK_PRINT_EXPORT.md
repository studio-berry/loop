# Quick print and export

Issue [#123](https://github.com/studio-berry/loop/issues/123) adds File → Export
document and File → Print to PDF. The approved scope for this PR is
print-to-PDF and PDF/PNG/TIFF exports. Native printer spooling remains outside
this slice; the product continues to exclude Qt PrintSupport and Widgets.

PDF export copies bytes without rendering or reserializing. For an accepted
correction, the Editor retains the exact bytes passed through Core's governed
publication gateway, together with its existing mutation receipt. Export checks
the receipt's published digest and binds the packet to the accepted revision.
An unmodified input is copied only if its on-disk digest still matches the
loaded document. An unpublished modified revision is refused.

Print to PDF renders every page through Core's `PDFTransparencyRenderer` and
`PDFRenderPolicy::forOutputPreview()`, then paints those new renders into a
QtGui PDF writer. PNG and TIFF render the current page through the same
compositor. These operations never read canvas pixels. They use Print or Export
optional-content usage respectively, preserve rotated media-page dimensions,
and offer 72–600 dpi. Restricted low-resolution printing is capped at 150 dpi.
The output is a color-managed RGB composite with separation simulation and
white paper; it is not a separations file. Annotation appearances are an
explicit fidelity limitation and require acknowledgment.

The dialog shows the current page's fidelity summary. An active warning, or a
limitation discovered during output rendering, requires explicit acknowledgment.
The result records renderer, resolution, color handling, page range, artifact
SHA-256, document revision, and any correction receipt. Output never certifies
print safety. A missing TIFF encoder is an explicit failure.

The scheduler prepares output in a private temporary directory. The owner
checks request identity, cancellation, lifecycle, and revision before Core's
atomic writer commits the selected destination. The writer verifies the staged
digest while copying. Cancelled, stale, failed, or refused requests preserve
an existing destination, and exporting over the input is refused.

`UnitTestsEditorHost` covers exact input and publication-byte copying, receipt
identity mismatch, stale requests, cancellation/replacement, output geometry,
raster dimensions and resolution, and overprint acknowledgment. Its existing
governed correction journey also exports the accepted artifact and verifies its
digest against the Core sign-off and mutation receipt.

Clean-machine print-to-file qualification on Windows and Linux is still a
separate acceptance lane. Record exact source SHA, fixture and artifact identity,
platform/toolchain, and hosted run before claiming that lane complete.

The final diff review retains Core's existing renderer, safe writer, receipt,
and command catalog. It introduces no renderer, publication authority, stored
receipt schema, dependency, or protected build-contract change.
