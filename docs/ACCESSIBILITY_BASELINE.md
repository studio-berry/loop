# Editor accessibility baseline

Loupe's Editor treats accessibility as part of the normal desktop workflow. The
baseline covers keyboard-only operation, deterministic menu mnemonics, visible
focus, screen-reader names and descriptions, semantic status text, contrast,
and DPI-aware sizing. It applies to the Editor shell and to Editor-hosted
workspaces such as Preflight.

## Shared policy

`Pdf4QtLibWidgets/sources/pdfaccessibility.*` is the shared policy and test
surface. It provides:

- widget-tree findings for controls that require explicit names or descriptions;
- menu mnemonic findings without rewriting user-facing action text at runtime;
- WCAG-style contrast ratio helpers (4.5:1 normal text, 3:1 large text and
  non-text focus boundaries);
- action-to-tool-button naming and font/style-derived spin-box sizing; and
- a registered `PDFDrawWidget` accessible interface with a privacy-safe page and
  zoom summary.

The canvas summary deliberately excludes file paths, extracted text, and
customer content. Document changes announce a value change through Qt's
accessibility event mechanism.

## Interaction rules

Primary controls must be reachable in a stable order with keyboard focus and
must expose a useful name. Status is expressed with text as well as color.
Finding tables and fixup controls expose their purpose, selection behavior, and
bounded-operation scope. Menu audits report missing or duplicate mnemonics for
developers; they do not silently change shortcuts or action labels.

`UnitTestsAccessibility` covers contrast targets, mnemonic diagnostics, control
names, action naming, and DPI-aware sizing. Visual/screen-reader verification
remains an application-level follow-up under the GUI/E2E harness issue.
