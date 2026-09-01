# Quick PDF parity

The Quick shell owns the interactive PDF surface; `LoupeLibCore` remains the
owner of PDF objects, document revisions, and persistence. The first parity
slice is intentionally model-driven:

- `QuickDocumentModel` exposes immutable page, outline, properties, attachment
  presence, optional-content presence, and revision values to QML.
- `QuickSearchResultModel` admits Core text-search results only when the
  captured `PDFRevisionIdentity` is still current.
- `DocumentPane.qml` provides pages, outline, search, next/previous result
  navigation, and the existing canvas in one Document workspace.
- Layout, fullscreen, find, and properties use the existing
  `CommandCatalog`; there is no QML action registry.

The following remain deliberately declared or policy-excluded until their
typed bridge and revision-fenced tests land: annotation/form overlays and
editing, attachments and metadata editing, print/export, undo/redo, password
and encryption workflows, sanitization, optimization, signature verification,
OCR, PageMaster, Compare, Redaction, signature creation, and deep inspection.

Search currently runs through the Core model on the host thread. It is a
functional read-only bridge, but its next hardening step is to submit the same
snapshot computation through `PDFJobScheduler` and admit the value on the
owner thread, matching the renderer and preflight paths.
