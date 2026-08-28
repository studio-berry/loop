Category: changed
Audience: developers
Breaking-Change: yes
Summary: Rebases the Loupe-to-Loop product rename onto current dev after Widgets retirement, preserving Phase 5 deletions while restoring PdfTool preflight test headers removed with the plugin pack. Phase 5 evidence generators and Release Gate verifiers now use Loop naming (`loop-release`, `docs/loop-shell.json`, `LOOP_*` CMake options). Phase 4 unit test targets `UnitTestsPageSurfaceBudget` and `UnitTestsDocumentViewSession` now link `LoopLib*` / `LoopEditorQuick` instead of leftover `Loupe*` names. Linux Release Gate now verifies the `loop-release` product-surface profile to match `LOOP_LOOP_DISTRIBUTION=ON` installs.
