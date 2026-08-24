# Loupe shell contract

This is the non-visual foundation for issue #193. The 0.1.1 release gate is
complete, but product GUI work remains gated by the S21 canvas and S22 Quick
admission contracts. This document therefore defines the state, routing, and
verification contract without changing the existing Widgets shell. The
repository may contain the qualification-only Quick smoke harness; it is not
product UI or a shipped Qt Quick surface.

The machine-readable source is [`loupe-shell.json`](loupe-shell.json), validated
by [`loupe-shell.schema.json`](schemas/loupe-shell.schema.json). The existing
Editor action inventory is recorded in [`loupe-shell-actions.json`](loupe-shell-actions.json).

## Product shell

`LoupeEditor` is the installed interactive Loupe shell on the P4-S7 navigable
product root: a packaged `Loupe.Quick` `ApplicationWindow` that opens, closes,
reopens, and navigates a PDF through the host-neutral Interaction/Canvas stack.
The Widgets editor remains available as the non-installed `LoupeEditorWidgetsOracle`
parity target until Phase 5. This is a navigable slice, not the Phase 4 operator
loop or GUI exit gate.

The repository may contain qualification-only Quick harnesses (`QuickShellSmoke`,
`CanvasBenchmark`); they are not product UI.

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

### Document status is a projection, not a second state machine

`pdfinteraction::DocumentFacade` owns the presentation-facing document
lifecycle. Its model is richer than the five status values above: a base state
(`Empty`, `Opening`, `Ready`, `Closing`, `Error`), independent facets (`Dirty`,
`Stale`, `Incomplete`, `Cancelled`, `Unsupported`), and a separate output axis
(`None`, `Pending`, `Saved`) — the document and production axes stay distinct,
as this contract requires.

The five document status values are a **pure projection** of that model, not a
parallel one a host maintains for itself:

| Facade state | Shell document status |
| --- | --- |
| any state other than `Ready` | `NO_DOCUMENT` |
| `Ready`, output `Pending` | `OUTPUT_PENDING` |
| `Ready`, `Dirty` | `MODIFIED` |
| `Ready`, output `Saved` | `OUTPUT_SAVED` |
| `Ready`, otherwise | `OPEN` |

The order is the precedence: a write in flight outranks unsaved changes, and
unsaved changes outrank a previous successful write, because the file on disk is
no longer what the operator is looking at. `DocumentFacade::projectShellStatus`
is the only implementation and `UnitTestsDocumentFacade` pins the whole table.

## Action and plugin policy

Every action declared in the current Editor `.ui` is listed in
`loupe-shell-actions.json` with a disposition and target group. The verifier
compares the policy against the 107 action IDs in
`LoupeLibGui/pdfeditormainwindow.ui`; missing or extra IDs fail the check.
This keeps the future shell from silently inventing routes.

Plugin actions follow the same policy. Their target group is determined by what
the action does, not by which plugin registers it. The plugin disposition table
in `loupe-shell.json` routes retained production plugins to Document,
Preflight, Production, Inspect, or Fix, while advanced and stopped plugins
remain outside the operator shell.

### The command catalog

Every action in `loupe-shell-actions.json` also carries a `command` object — the
host-neutral descriptor `pdfinteraction::CommandCatalog` loads. There is one
registry, not a Quick-side copy: the descriptors live in the same file whose ID
set is already pinned against the Editor `.ui`, so a command that is not a
declared Editor action cannot exist.

| Field | Meaning |
| --- | --- |
| `label_key` | Translation key, always `command.<id>.label` |
| `shortcut` | A `QKeySequence::StandardKey` name or a literal sequence, with an optional `windows` override. Resolution into a key sequence is the presentation host's job |
| `parameters` | Typed parameter specs; unknown or mistyped parameters are refused, never defaulted |
| `capability` | What the command may touch. `unclassified` is permitted only while the command is `declared` |
| `cancellable` | Whether the command supports cancellation |
| `availability` | `implemented` (a handler exists) or `declared` (descriptor only) |

A `declared` command is not a gap in the contract. It has a complete descriptor,
so menus, shortcuts, and routing are complete, and invoking it returns the typed
`not-implemented` terminal state while mutating nothing. Promoting one to
`implemented` requires a real capability and a registered handler; invoking an ID
that is not in the contract is reported as a routing error rather than ignored.

`scripts/verify-command-catalog.py` checks the block, and checks shortcut parity
against `PDFActionManager::initActions` so the catalog cannot become a second
command truth wearing the first one's ID set.

**Phase 5 note:** this file derives its ID set from
`LoupeLibGui/pdfeditormainwindow.ui`. When Phase 5 deletes that form, the parity
check in `verify-loupe-shell-contract.ps1` loses its source and this file must
become self-authoritative.

## UI foundation gate

Issue #178 selects Qt Quick Controls for the later application shell. The
decision is accepted with admission gates. No product Qt Quick module, product
QML file, `QQuickWidget`, or shell restyle is introduced by this slice; the
optional `QuickShellSmoke` target is qualification-only. ADR-010 and the
Quick policy contracts resolve the migration strategy, canvas hosting,
rendering fallback, licensing, and accessibility evidence required before
large-scale shell wiring begins.

## Accessibility gate

When the GUI gate opens, workspace switching and rail/canvas/inspector traversal
must be keyboard reachable, every panel must expose a screen-reader name and
role, focus must survive workspace changes, and 100/150/200% high-DPI layouts
must remain usable. Contrast and severity colors follow #25 and #194; this
contract does not create a second accessibility baseline. P4-S10 evidence lives
in [QUICK_ACCESSIBILITY_CONTRACT.md](QUICK_ACCESSIBILITY_CONTRACT.md) and
`scripts/run-product-quick-a11y-smoke.ps1`.

## Verification

Run the static contract check from the repository root:

```powershell
pwsh ./scripts/verify-loupe-shell-contract.ps1
```

This verifies JSON shape, workspace parity with the #192 product-surface
manifest, plugin disposition targets, and complete Editor action coverage. It
does not claim GUI behavior, accessibility runtime success, or product Qt Quick
rendering validation; those remain S21/S22 and later runtime/package gates.

The command descriptors have their own check:

```bash
python scripts/verify-command-catalog.py
```

`UnitTestsDocumentFacade` covers the runtime side — catalog loading, invocation
and cancellation, and the document lifecycle — with no `QWidget` and no QML
engine.
