# Drag-commit decision (ADR-012)

Category: internal
Audience: developers
Breaking-Change: no
Summary: Record ADR-012, the decision package for issue #104. A completed drag is emitted as a DragSession and discarded by EditorHost::onDragCompleted; committing it needs a move/translate command that does not exist among the 107 IDs in the schema-validated docs/loop-shell-actions.json, and a document mutation under protected LoopLibCore/sources/**. The ADR answers the issue's four open questions (command identity and granularity, payload shape, undoability and revision fencing, interaction with actionUndo/actionRedo), recommends one kind-agnostic actionMoveSelection over a page-space delta executed through a new governed Core repair operation, and states the exact contract deltas (action entry, expected_action_count 107 to 108, implemented-set registration, the new operation's public surface, and DragSession revision exposure). No production source or schema changed.
