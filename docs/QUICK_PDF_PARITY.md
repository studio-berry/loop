# Quick competitor parity

The Quick shell owns the interactive PDF surface; `LoopLibCore` remains the
owner of PDF objects, document revisions, and persistence. The first parity
slice is intentionally model-driven:

- `QuickDocumentModel` exposes immutable page, outline, properties, capability,
  lifecycle, attachment presence, optional-content presence, and revision values
  to QML.
- `QuickSearchResultModel` admits Core text-search results only when the
  captured `PDFRevisionIdentity` is still current.
- `DocumentPane.qml` provides pages, outline, search, next/previous result
  navigation, and the existing canvas in one Document workspace.
- Layout, fullscreen, find, and properties use the existing
  `CommandCatalog`; there is no QML action registry.
- Encrypted opens use Core's security handler through a worker callback and a
  masked, keyboard-accessible QML prompt. Incorrect and cancelled passwords
  produce `document/password-incorrect` and `document/password-cancelled`;
  retry creates a new request. Close, replacement, and teardown cancel the
  waiting worker, and replies to retired request IDs are refused.
- The facade projects Core's print, modify, copy, and assemble permissions.
  Restricted print/copy commands remain unavailable. Correction planning,
  approval, rollback, and saving require all four permissions;
  this conservative gate offers no restriction override. The worker and writer
  also refuse restricted corrections before producing a candidate or output.
- Inspection receipts retain encryption state in their existing limitation
  text. Quick's preflight audit stores the Core receipt in its result summary.
  Passwords remain transient prompt/reader inputs and never enter those records.

The following remain deliberately declared or policy-excluded until their
typed bridge and revision-fenced tests land: annotation/form overlays and
editing, attachments and metadata editing, print/export, encryption
creation or changes, sanitization, optimization, signature verification,
OCR, PageMaster, Compare, Redaction, signature creation, and deep inspection.

Search currently runs through the Core model on the host thread. It is a
functional read-only bridge, but its next hardening step is to submit the same
snapshot computation through `PDFJobScheduler` and admit the value on the
owner thread, matching the renderer and preflight paths.

Quick Undo and Redo follow [governed revision history](QUICK_REVISION_HISTORY.md).
