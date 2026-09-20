Category: internal
Audience: developers
Breaking-Change: no
Summary: Decompose EditorHost preflight/inspector/menu glue and split RGB-to-CMYK image fixup ahead of PR #572 promotion. Preflight profile catalog and run submitter move to LoopLibInteraction; menu groups are manifest-driven via `menu_group` in loop-shell-actions.json; inspector dispatch extracts to ShellInspectorDispatch without pending placeholders; pdfrgbtocmyk image conversion moves to pdfrgbtocmykimagefixup.cpp.
