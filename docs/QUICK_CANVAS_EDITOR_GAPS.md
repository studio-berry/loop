# Quick canvas-editor gaps: Select/Hand tools and drag commit

Two gaps found while planning the canvas-editor GUI work. Gap 1 is now fixed by
the tool-vocabulary work for issue #103; gap 2 remains open and is tracked as
issue #104.

Verified against `origin/dev` at `e9953734`.

## 1. The Select and Hand toolbar buttons do nothing — fixed (#103)

`LoopEditor/qml/ShellToolBar.qml` presented `Select` and `Hand` as checkable
tool buttons. Neither had an `onClicked` handler, neither was bound to
`activeTool`, and the two could be checked independently.

Wiring the buttons to the existing setter alone would have made two dead
controls look live while changing no observable behavior: `InteractionController`
never read `m_activeTool` to branch pointer behavior. Issue #103 therefore
required a vocabulary and real Hand/Select pointer semantics, not QML wiring.

### How it was fixed

- The vocabulary is defined in [INTERACTION_CONTRACT.md](INTERACTION_CONTRACT.md)
  under "Tools (issue #103)" and typed as `pdfinteraction::InteractionTool` in
  `LoopLibInteraction/sources/interactionstate.h`. A name outside the vocabulary
  is refused at the host boundary; it is never coerced into a default.
- `InteractionController::handlePointerPress` routes a left press to
  `InteractionKind::Pan` when the active tool is Hand and otherwise keeps the
  Select selection/drag path; `handlePointerRelease` ends a Hand pan on the left
  button. Select is the default and retains the pre-vocabulary behavior.
- `EditorHost` exposes an `activeTool` property and a `setActiveTool` invokable
  that parses the name once and routes the parsed tool to the session's
  interaction controller.
- `LoopEditor/qml/ShellToolBar.qml` binds both buttons' `checked` to
  `host.activeTool` inside an exclusive `ButtonGroup`, so they are a
  single-select that reflects the real tool.
- Cancel-on-change stays as it was: `setActiveTool` still calls
  `cancelActive(InteractionCancelReason::ToolChanged)` (issue #141 AC3), so a
  tool change mid-drag drops the gesture and commits nothing.

Coverage: `UnitTestsInteractionController` slots
`handToolPansLeftDragAndSuppressesSelection`, `selectToolRetainsSelectionAndDrag`,
`toolVocabularyNamesRoundTrip` and `toolChangeCancelsDrag`;
`UnitTestsQuickCanvas::toolSelectionRoundTrips`;
`UnitTestsShellKeyboard::activeToolIsExposedAndValidated`; and
`verifyToolSelection` in `tools/ProductQuickAccessibilitySmoke/main.cpp`, which
reads the live buttons' `checked` state back after driving the host.

## 2. Completed drags are discarded

`InteractionController` emits exactly one `dragCompleted(DragSession)` per
completed drag and deliberately leaves the commit to its owner. The Quick host
is that owner, and it drops the session.

`LoopEditor/editorhost.cpp`:

```cpp
void EditorHost::onDragCompleted(pdfinteraction::DragSession session)
{
    Q_UNUSED(session);
    if (m_session->interaction())
    {
        m_session->interaction()->refreshOverlay();
    }
}
```

An operator can select a finding or object, drag it, watch the preview move,
and have nothing committed.

### Why it is a contract change, not a patch

`docs/INTERACTION_CONTRACT.md` states that the owner routes the session
through `CommandCatalog`, "which stays the only mutation path." The command it
names was never added. All 107 IDs in `docs/loop-shell-actions.json` are
navigation, create, color, bookmark, or render actions; none is a move or
translate.

- `docs/loop-shell-actions.json` is validated by the protected schema
  `docs/schemas/loop-shell-actions.schema.json` and declares
  `expected_action_count: 107`, which the action list must match.
- The mutation itself would land in `LoopLibCore/sources/**`, listed under
  `protected_paths` in `agent-policy.json`.

Per `AGENTS.md`, a required change to a protected schema or central type is
reported rather than invented.

## Why no code accompanies gap 2

Gap 2 needs a decision that a patch cannot make: it is a new public contract
entry with undo, revision-fencing, and payload questions still open. The tool
plumbing prototyped and reverted while writing this document — an `activeTool`
property and a `setActiveTool` invokable on `EditorHost` — is exactly what gap
1's fix now re-derives, because the semantics it needed exist.

Refs #103, #104
