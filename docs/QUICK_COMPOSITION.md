# Qt Quick composition pattern

This document defines how Loupe product components will be composed when the
Qt Quick foundation is introduced. It is a contract and example, not a QML
implementation. The repository intentionally remains QML-free until the
post-0.1.1 Quick slice is approved.

## Ownership boundary

Product components have three layers:

| Layer | Owns | Must not own |
| --- | --- | --- |
| C++ document/session model | PDF document, page/render state, preflight reports, operation history, approvals, mutations | control styling or transient popup geometry |
| C++ command boundary | typed commands, validation, cancellation, busy state, errors, accessibility-facing status text | direct widget/QML object graphs |
| QML product component | layout, control composition, focus scopes, menu/dialog/popup state, visual state bindings | PDF mutation, file writes, duplicate history, private document copies |

QML receives read-only state and invokes named commands. A command returns a
deterministic result or an observable pending state; a component never edits a
PDF by changing a local property and hoping the model catches up.

## Component shape

The following pseudocode shows the intended shape for an approval-gated
production action. It uses Qt Quick Controls behavior while leaving visual
style to the application theme.

```qml
Item {
    id: productionSurface
    required property var documentState
    required property var operationController

    SplitView {
        anchors.fill: parent

        PageCanvasHost {
            SplitView.fillWidth: true
            document: productionSurface.documentState.document
        }

        Pane {
            implicitWidth: 360

            TabBar {
                id: tabs
                TabButton { text: qsTr("Findings") }
                TabButton { text: qsTr("History") }
                TabButton { text: qsTr("Preview") }
            }

            StackLayout {
                currentIndex: tabs.currentIndex
                FindingsView { model: documentState.findings }
                OperationHistoryView { model: documentState.history }
                ProductionPreviewView { state: documentState.preview }
            }
        }
    }

    Dialog {
        id: approvalDialog
        title: qsTr("Approve production operation")
        standardButtons: Dialog.Ok | Dialog.Cancel

        onAccepted: operationController.approvePending()
        onRejected: operationController.rejectPending()
    }

    Connections {
        target: operationController
        function onApprovalRequired() { approvalDialog.open() }
    }
}
```

The example is intentionally a pattern rather than a copy-paste screen:

- `PageCanvasHost` is the one canvas boundary selected by ADR-007.
- `FindingsView`, `OperationHistoryView`, and `ProductionPreviewView` bind to
  model-owned state; they do not run preflight, rollback, or file writes.
- `Dialog` is opened by an approval state transition, not by a button that
  bypasses the operation controller.
- The visual theme can replace control styling without changing command or
  document behavior.

## Control selection

Use the smallest semantic control that expresses the operation:

| Product need | Quick Controls primitive | Required behavior |
| --- | --- | --- |
| approval gate | `Dialog` | explicit title, initial focus, accept/cancel, Escape cancellation, restore focus |
| page/finding actions | `Menu` / `MenuItem` / `ContextMenu` | keyboard typeahead, deterministic order, accessible names, dismissal |
| anchored evidence | `Popup` / `ToolTip` | anchor survives model updates, Escape closes, focus does not disappear |
| workspace navigation | `TabBar` / `TabButton` / `StackView` | selected state is exposed without relying on color |
| long findings/history | `ScrollView` plus a virtualized view | bounded delegate creation and preserved keyboard selection |
| view state | `Switch`, `CheckBox`, or `RadioButton` | state text and accessible name describe the effect |
| command strip | `ToolBar` / `ToolButton` | tooltip is supplementary; name and keyboard action are primary |

Custom controls are justified only when the semantic primitive cannot express
the interaction. A custom control must document its keyboard, focus, name,
description, and state behavior next to the component.

## Keyboard and focus contract

Every dialog, menu, and popup must have an executable interaction test before
it ships:

1. Open it using the keyboard from a known focused control.
2. Move through every actionable item with `Tab`/`Shift+Tab` and the control's
   standard arrow/typeahead keys.
3. Show a visible focus indicator that is not conveyed by color alone.
4. Confirm the accessible name, role, description, and current state.
5. Confirm `Escape` dismisses the transient surface or cancels the pending
   approval without mutating the document.
6. Confirm focus returns to the invoking control after dismissal.
7. Repeat the sequence across the Widgets/Quick boundary during mixed mode.

Focus transfer must be explicit at the bridge. The invoking QWidget or QML
`Item` records the intended return target; the adapter transfers focus once
when opening and once when closing. Do not depend on destruction/recreation or
window activation to restore focus.

## Rendering and model rules

- Bind to immutable snapshots or notifier-backed properties; do not bind a
  delegate directly to a mutable PDF object graph.
- Keep document work, rasterization, and preflight off the QML render thread.
- Expose progress, cancellation, and failure as state with text, not only an
  animated busy indicator.
- Keep the canvas host replaceable so the mixed-mode `PDFDrawWidget` and final
  scene-graph adapter share the same document/session contract.
- Do not introduce a second history, undo, or approval store in QML.

## First-slice verification

The first Quick surface must include:

- a keyboard/focus test covering its dialogs, menus, and popups;
- a software-backend smoke run with `QSG_RHI_BACKEND=software` on Windows and
  Linux, in addition to each platform's preferred backend;
- package inspection proving the actual Qt Quick/QML runtime modules and
  licenses are present and no web runtime was added; and
- a bridge test proving a QWidget-to-Quick-to-QWidget focus round trip.

Until those checks exist, this composition pattern remains a design contract;
it does not authorize adding QML or Qt Quick modules to the shipped artifact.
