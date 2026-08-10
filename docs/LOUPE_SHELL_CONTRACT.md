# Loupe shell contract

This is the non-visual foundation for issue #193. Per the product scope gate,
GUI elements remain deferred until after 0.0.3. This document therefore defines
the state, routing, and verification contract without changing the existing Qt
Widgets shell or adding QML.

The machine-readable source is [`loupe-shell.json`](loupe-shell.json), validated
by [`loupe-shell.schema.json`](schemas/loupe-shell.schema.json). The existing
Editor action inventory is recorded in [`loupe-shell-actions.json`](loupe-shell-actions.json).

## Product shell

`Pdf4QtEditor` remains the only interactive Loupe shell. `PdfTool` remains Loupe
CLI. Opening a PDF is the Document workspace and includes inherited Viewer
behavior. PageMaster, Diff, and Viewer are not new windows in this contract;
their retained Core/CLI semantics are routed into the product workspaces when
the GUI gate opens.

The eventual shell has these workspace IDs:

| Workspace | Semantic owner | Operator state preserved |
| --- | --- | --- |
| Document | Editor document/session | document, preflight |
| Preflight | Core preflight + PdfTool contract | document, preflight |
| Production Preview | render/color/preview contracts | document, preflight |
| Pages / Production | `PDFPageMasterExport`, ADR-003, ADR-004 | document, preflight |
| Inspect | Core inspection + contextual plugins | document, preflight |
| Fix | bounded Core/PdfTool operations | document, preflight |
| Compare | Core `PDFDiff`, pending product decision | document, preflight |

The eventual composition is intentionally recorded as a contract rather than
implemented UI:

```text
toolbar: open · save/export · undo/redo · select/hand · zoom · preflight · preview
workspace rail | PDF canvas | contextual inspector
status: document state · production state · preflight state
```

The canvas remains the existing PDF rendering surface. The inspector is a
single context dispatcher for page, image, finding, separation, and empty-canvas
selection; it must not grow one independently-owned panel per plugin.

| Selection | Inspector contract |
| --- | --- |
| Page | boxes, size, rotation, and OCG context |
| Image | effective/native DPI, colour space, compression, and mask |
| Finding | linked evidence and Prepress Inspector context |
| Separation | process/spot identity and ink-coverage context |
| Empty canvas | document summary and preflight status |

Switching workspace is a context change only. It must not close the document,
discard a report, or detach a report from its document revision. The shell owns
navigation and context selection; Core and PdfTool remain the semantic owners
of PDF operations.

## State and status

The shell keeps document, production, and preflight state distinct. The status
bar must make these states visible without opening a dialog when GUI work is
eventually enabled.

- Document: `NO_DOCUMENT`, `OPEN`, `MODIFIED`, `OUTPUT_PENDING`, `OUTPUT_SAVED`.
- Production: `NOT_READY`, `READY`, `OPERATION_PENDING`,
  `APPROVAL_REQUIRED`, `OUTPUT_WRITTEN`.
- Preflight: `NOT_CHECKED`, `RUNNING`, `CANCELLED`, `PASS`, `FINDINGS`,
  `STALE`, `INCOMPLETE`.

`NOT_CHECKED`, `STALE`, and `INCOMPLETE` are explicit states, never empty UI or
implicit success. A correction that changes the bound document revision makes
the prior preflight result `STALE` until revalidation completes.

## Action and plugin policy

Every action declared in the current Editor `.ui` is listed in
`loupe-shell-actions.json` with a disposition and target group. The verifier
compares the policy against the 107 action IDs in
`Pdf4QtLibGui/pdfeditormainwindow.ui`; missing or extra IDs fail the check.
This keeps the future shell from silently inventing routes.

Plugin actions follow the same policy. Their target group is determined by what
the action does, not by which plugin registers it. The plugin disposition table
in `loupe-shell.json` routes retained production plugins to Document,
Preflight, Production, Inspect, or Fix, while advanced and stopped plugins
remain outside the operator shell.

## UI foundation gate

Issue #178 proposes Qt Quick Controls for the later application shell, but it is
still open. The contract records Qt Quick Controls as the candidate and keeps
the decision pending. No Qt Quick module, QML file, `QQuickWidget`, or shell
restyle is introduced by this slice. Once the 0.0.3 GUI gate opens, #178 must
resolve the migration strategy, canvas hosting, rendering fallback, licensing,
and accessibility baseline before large-scale shell wiring begins.

## Accessibility gate

When the GUI gate opens, workspace switching and rail/canvas/inspector traversal
must be keyboard reachable, every panel must expose a screen-reader name and
role, focus must survive workspace changes, and 100/150/200% high-DPI layouts
must remain usable. Contrast and severity colors follow #25 and #194; this
contract does not create a second accessibility baseline.

## Verification

Run the static contract check from the repository root:

```powershell
pwsh ./scripts/verify-loupe-shell-contract.ps1
```

This verifies JSON shape, workspace parity with the #192 product-surface
manifest, plugin disposition targets, and complete Editor action coverage. It
does not claim GUI behavior, accessibility runtime success, or Qt Quick
rendering validation; those remain post-0.0.3 gates.
