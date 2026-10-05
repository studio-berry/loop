# Select and Hand tools carry real behavior (#103)

Category: added
Audience: developers
Breaking-Change: no
Summary: Define the closed Select/Hand tool vocabulary in the interaction contract and as `pdfinteraction::InteractionTool`, route a Hand left-drag to panning while Select keeps selection and drag, refuse unknown tool names at the host boundary, expose `EditorHost::activeTool`/`setActiveTool`, and bind the shell toolbar's Select/Hand buttons to it as a single-select inside an exclusive `ButtonGroup`. Covered by failing-first interaction and quick tests plus a ProductQuickAccessibilitySmoke check that reads the live buttons' checked state.
