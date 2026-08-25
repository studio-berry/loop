# Quick accessibility contract

Status: P4-S10 (0.2.0 Phase 4). Types live in `LoupeLibQuick/sources/loupecanvasaccessible.*`,
`LoupeEditor/focusrestoration.*`, and the packaged `Loupe.Quick` shell QML.

## Scope

P4-S10 completes keyboard, focus, accessibility, and reduced-motion behavior for the
product Quick path. It extends the Widgets baseline documented in
[ACCESSIBILITY_BASELINE.md](ACCESSIBILITY_BASELINE.md) without creating a second standard.

Phase 6 owns Windows screen-reader and Linux accessibility smoke as release proof.
P4-S10 delivers product-runtime hooks and automated smoke evidence only.

## Binding rules

1. The document canvas exposes exactly one `QAccessible::Canvas` node with a privacy-safe
   summary. Tile nodes are not accessible objects.
2. Every workspace panel, rail control, status surface, and dialog records an explicit
   accessible name and role.
3. Transient surfaces restore focus to the invoking control on accept, reject, or dismiss.
4. Command shortcuts come from `CommandCatalog`; QML does not duplicate catalog shortcuts.
5. Reduced motion disables nonessential transitions without changing operation semantics.
6. Status is announced with text as well as color.

## Verification

| Check | Command / target |
| --- | --- |
| Canvas accessible interface | `UnitTestsQuickAccessibility` |
| Shell keyboard helpers | `UnitTestsShellKeyboard` |
| Product smoke (native + software) | `scripts/run-product-quick-a11y-smoke.ps1` |
| Static policy | `scripts/verify-quick-shell-policy.py` |

## Architecture invariant

**I27** — The Quick canvas accessible tree has no tile-level children; one canvas node
summarizes page and zoom state without file paths or extracted text.
