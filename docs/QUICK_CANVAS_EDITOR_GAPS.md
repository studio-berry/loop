# Quick canvas-editor gaps: Select/Hand tools and drag commit

Two gaps found while planning the canvas-editor GUI work, recorded here so the
next session does not have to rediscover them. Neither is fixed; both are
tracked as issues #103 and #104. No production source is changed by this
document.

Verified against `origin/dev` at `e9953734`.

## 1. The Select and Hand toolbar buttons do nothing

`LoopEditor/qml/ShellToolBar.qml:121-135` presents `Select` and `Hand` as
checkable tool buttons. Neither has an `onClicked` handler, neither is bound to
`activeTool`, and the two can be checked independently.

### The obvious fix is wrong

`InteractionController::m_activeTool` is currently **write-only**. Across
`LoopLibInteraction` and `LoopLibQuick`, every reference is:

| Location | Use |
| --- | --- |
| `interactioncontroller.cpp:106` | equality guard on set |
| `interactioncontroller.cpp:111` | assignment |
| `interactioncontroller.h:113` | inline getter |
| `interactioncontroller.h:208` | declaration |
| `loopcanvasitem.cpp:190-197` | getter, and an equality guard on set |
| `loopcanvasitem.h:100,141,162` | property, declaration, notify signal |

Nothing reads the value to branch behavior. `handlePointerPress`
(`interactioncontroller.cpp:226-268`) routes pan to `m_panButton`, which
defaults to middle mouse, and routes selection and drag off pointer button,
modifiers, and `InteractionTargetKind`. The active tool is not consulted.

`LoopCanvasItem::activeTool` is also declared `READ` only
(`loopcanvasitem.h:100`), so the C++ setter has never been reachable from QML.

Wiring the buttons to the existing setter would make two dead controls look
live while changing no observable behavior. That is a worse defect than the
current honest-but-inert one, because the operator loses the ability to tell
the feature is absent.

### What a real fix needs

1. A tool vocabulary defined in a contract, not implied by control labels. Tool
   IDs are free-form today; the only test uses `"measure"`
   (`UnitTests/tst_interactioncontrollertest.cpp:448`), which no production path
   sets.
2. Hand/select pointer semantics: Hand claims left-drag for panning and
   suppresses selection and drag initiation; Select retains today's behavior.
3. Cancel-on-change stays as it is. `setActiveTool` already calls
   `cancelActive(InteractionCancelReason::ToolChanged)` (issue #141 AC3), so a
   tool change must not leave a half-applied transform.

This is a missing feature rather than a defect, and the behavior change lands in
the `interaction` boundary.

## 2. Completed drags are discarded

`InteractionController` emits exactly one `dragCompleted(DragSession)` per
completed drag and deliberately leaves the commit to its owner. The Quick host
is that owner, and it drops the session.

`LoopEditor/editorhost.cpp:3579-3586`:

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

`docs/INTERACTION_CONTRACT.md:33-34` states that the owner routes the session
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

## Why no code accompanies this document

Both gaps need a decision that a patch cannot make. The tool semantics are a
feature design; the drag command is a new public contract entry with undo,
revision-fencing, and payload questions still open. The plumbing that would
carry the tool choice — an `activeTool` property and a `setActiveTool` invokable
on `EditorHost`, routing through the attached canvas to the existing controller
— was prototyped and reverted, because on its own it is a no-op. It is
straightforward to re-derive once the semantics exist.

## Local verification limit

The mapped test lanes cannot be run in this checkout. `build-local/` does not
exist, and `C:/.dev/repos/loop/.local-vcpkg/` — the `CMAKE_TOOLCHAIN_FILE`
referenced by every build cache under `C:/.dev/build/` — has been removed.
Restoring vcpkg and configuring are both approval-required under
`agent-policy.json`. This document is therefore unproven by build or test; it
records findings from source reading and search, and each claim above cites the
file and line it was read from.

Refs #103, #104
