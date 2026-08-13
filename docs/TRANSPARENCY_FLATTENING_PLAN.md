<!--
MIT License

Copyright (c) 2018-2025 Jakub Melka and Contributors
-->

# Transparency flattening contract

Issue #164 uses one shared Core operation for every production surface:
`PDFTransparencyFlattener::apply()`.

The first production-safe mode rasterizes each selected page through
`PDFTransparencyRenderer` onto an opaque image. This preserves the renderer's
handling of blend modes, transparency groups, soft masks, knockout, and
isolation while keeping the operation deterministic across PdfTool and
PageMaster. The rasterization report records the page bounds and the reason
for every rasterized region.

The operation is deliberately headless. Editor controls are deferred until
after 0.1.1, but the Core settings already carry the raster DPI, line-art/text
policy, spot-color policy, page range, dry-run mode, and raster-pixel budget.

The locked PageMaster order is:

`assemble -> preflight -> page geometry -> bleed fixup -> transparency flatten -> image optimize -> write`

After a write-capable operation, the Core operation rechecks the document for
live transparency. Dry runs produce the same report without changing the
source document.

The current implementation intentionally reports the entire page as the
rasterized region. A later vector-preserving balance mode can use the existing
settings and report contract without introducing a second pipeline.
