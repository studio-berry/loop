# Editor accessibility baseline

Loupe's Editor treats accessibility as part of the normal desktop workflow. The
baseline covers keyboard-only operation, deterministic menu mnemonics, visible
focus, screen-reader names and descriptions, semantic status text, contrast,
and DPI-aware sizing. It applies to the Editor shell and to Editor-hosted
workspaces such as Preflight.

## Shared policy

`LoupeLibWidgets/sources/pdfaccessibility.*` is the shared policy and test
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

## Qt Quick extension for 0.2.0

ADR-007 adopts Qt Quick Controls as the 0.2.0 shell foundation. It extends this
baseline; it does not create a second accessibility standard. Quick components
must expose the same meaningful name, description, role, state, visible focus,
keyboard reachability, contrast, status text, and DPI-aware sizing expected of
Widgets components.

Every Quick `Dialog`, `Menu`, and `Popup` must have a keyboard/focus test that
covers opening, traversal, typeahead where applicable, Escape dismissal,
approval cancellation, and focus restoration. During mixed mode, the test
must cross the QWidget/Quick boundary in both directions. The composition
rules and test shape are documented in [QUICK_COMPOSITION.md](QUICK_COMPOSITION.md).

No product Qt Quick module or QML surface is shipped by this decision record.
S22 adds only a qualification harness plus static token and boundary checks.
The first product implementation must add screen-reader, focus-bridge, and
software-renderer evidence to the same release gate used for the existing
Editor baseline.
