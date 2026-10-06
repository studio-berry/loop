Category: changed
Audience: operators
Breaking-Change: no
Summary: A completed drag of a finding, guide or handle is no longer silently dropped. The Editor now refuses it with an announced reason and a `dragRefused` signal, and leaves the document, selection and overlay unchanged, while a page-box drag still commits once through `actionMoveSelection`. Text has no target kind, so no drag can start on it. The per-family decision is recorded in ADR-012, and `getDragCommitDisposition` is the single answer for each target kind. Reviewer can check `onlyAPageBoxDragIsAdmittedForCommit` and `completedDragOfARefusedKindIsReportedAndChangesNothing`.
