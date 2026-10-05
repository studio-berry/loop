# Quick accessibility contract

Status: P4-S10 (0.2.0 Phase 4). Types live in `LoopLibQuick/sources/loopcanvasaccessible.*`,
`LoopEditor/focusrestoration.*`, and the packaged `Loop.Quick` shell QML.

## Scope

P4-S10 completes keyboard, focus, accessibility, and reduced-motion behavior for the
product Quick path. It extends the Widgets baseline documented in
[ACCESSIBILITY_BASELINE.md](ACCESSIBILITY_BASELINE.md) without creating a second standard.

Phase 6 owns Windows screen-reader and Linux accessibility smoke as release proof.
P4-S10 delivers product-runtime hooks and automated smoke evidence only. The
installed-package qualification below is the release proof; it runs against the
installed tree, and the native OS accessibility backend is required where the
platform provides one.

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
| Installed-tree operator path, native backend | `scripts/run-product-quick-a11y-smoke.ps1 -InstallTree <install> -Backend native` |
| Installed-tree operator path, software rasterizer | `scripts/run-product-quick-a11y-smoke.ps1 -InstallTree <install> -Backend software` |
| Native OS accessibility backend (Windows) | `scripts/run-installed-quick-a11y-uia.ps1` |
| Fail-closed qualification gate | `scripts/ci/verify_quick_accessibility_evidence.py` |
| Static policy | `scripts/verify-quick-shell-policy.py` |

The package workflows (`LinuxInstall.yml`, `WindowsInstall.yml`) run the
qualification against the **installed** tree after `cmake --install` and before
packaging, not against the developer build tree:

- the native and software runs write separate records (`quick-a11y-native.json`,
  `quick-a11y-software.json`) with distinct claims, and their text transcripts
  stay separate as before;
- Windows additionally drives the native UI Automation client and records
  `quick-a11y-native-uia.json`; that lane is required, so the native
  accessibility backend is proven active against the installed closure;
- Linux records the native-accessibility lane as unavailable
  (`quick-a11y-native-accessibility-unavailable.txt`) because a headless runner
  has no AT-SPI accessibility bus, instead of leaving it absent-and-fine;
- `verify_quick_accessibility_evidence.py` fails closed when a required lane is
  missing, failed, captured outside an installed tree, or replaced by a
  software-only record. A software-only smoke can never satisfy a native claim.

## Architecture invariant

**I27** — The Quick canvas accessible tree has no tile-level children; one canvas node
summarizes page and zoom state without file paths or extracted text.
