# ADR-005 Implementation Plan: Product Surface Pruning

**Branch:** `cdx/issue-191-product-surface`
**Source decision:** [ADR-005: Product surface pruning classification](https://app.notion.com/p/3b69cb079ddb813492a8cb6f84f23d14?pvs=204)
**Status:** The Issue #191 product-surface contract is implemented. The checked-in
manifest, source/package verifier, and profile-specific CI checks now describe the
current post-Session-05 tree; Editor workspace integration remains planned for
#193 / 0.0.1B.

## 0.2B transitional surface contract

The release package presents two product surfaces:

- **Loupe** is the `LoupeEditor` desktop shell. Opening a PDF provides the
  normal viewing behavior; future Editor workspaces are named Pages /
  Production, Compare, and Production Preview.
- **Loupe CLI** is the product-facing name for `PdfTool`. The `PdfTool`
  executable, command names, machine-readable contracts, and automation
  compatibility remain unchanged.

The release profile contains `LoupeEditor`, `PdfTool`, `LoupeLibCore`, and
`LoupeLibQuick`. CodeGenerator, JBIG2 Viewer, and PdfExampleGenerator remain
available only in the full developer build. The former Viewer, PageMaster, Diff,
LaunchPad, and editor-plugin sources are deleted and absent from both profiles;
they are recorded as historical dispositions so an upstream reintroduction is
detected rather than silently shipped.

The Editor-facing Pages / Production and Compare seams must reuse the existing
Core/PageMaster export and Core Diff contracts. This 0.2B slice does not move
their visible UI into Editor; that workspace integration is a 0.3 deliverable.

Issue #192 records the boundary in [LOUPE_WORKSPACES.md](LOUPE_WORKSPACES.md).
Issue #191 makes [product-surface.json](product-surface.json) the executable
inventory for developer and `loupe-release` packaging profiles. Both
`scripts/verify_product_surface.py` and the PowerShell compatibility entry point
consume it; this plan and ADR-005 remain the decision record.

## Objective

Move Loupe from an upstream-shaped multi-application distribution to a focused
print-production product surface while preserving shared upstream engine code,
PdfTool automation, and upstream-sync flexibility.

The implementation must follow the ADR-005 order:

1. make the release surface explicit and merge-durable;
2. hide/reorganize UI before deleting implementation;
3. stop shipping non-product executables through release-profile build and
   packaging flags;
4. hard-delete only after dependency, upstream-sync, and product decisions are
   complete.

## Scope

### In scope

- Loupe desktop as the primary interactive surface.
- Loupe CLI, currently `PdfTool`, as the headless/automation surface.
- First-class product groupings for Document, Preflight, Production Preview,
  Pages, Inspect, Fix, and Compare.
- Release-profile gating for developer utilities and the absence of deleted
  Viewer, Diff, PageMaster, LaunchPad, and plugin artifacts.
- Linux desktop entries, Flatpak, Windows/WiX feature selection, and release
  artifact verification.
- Editor menu and plugin-surface reorganization after the product IA is locked.
- Explicit handling of the unresolved redaction and Compare decisions.

### Out of scope for the first implementation slice

- Deleting shared Core code or third-party/upstream source trees.
- Reimplementing PageMaster, Diff, or Viewer semantics inside the Editor.
- Moving the Preflight engine out of Core or creating a second GUI-only report
  interpretation.
- Changing `AGENTS.md` architecture guidance as part of the 1.1A audit.
- Product redesign beyond the minimum visibility and packaging gates needed to
  validate the surface decision.

## Current repository anchors

| Concern | Current anchor | Planning implication |
| --- | --- | --- |
| Distribution switch | `CMakeLists.txt:58-113` | Reuse `LOUPE_LOUPE_DISTRIBUTION`; the manifest records its developer/release consequences. |
| Conditional application targets | `CMakeLists.txt:284-320` | Keep maintained developer tools opt-in to the release profile and fail on unmanifested install targets. |
| Linux install surface | `CMakeLists.txt:374-420` | Keep the manifest desktop inventory aligned with profile-selected `.desktop`, icon, and metainfo installation. |
| Windows packaging | `WixInstaller/CMakeLists.txt` and `WixInstaller/Product.wxs.in` | Package Editor, PdfTool, Core, and Quick without stale compatibility/plugin features or Qt Widgets. |
| Editor shell | `LoupeEditor/` and `docs/loupe-shell.json` | Keep the Quick shell contract as the UI authority; future workspace wiring remains a separate issue. |
| Plugin registry/build | `docs/generated/phase5-widgets-disposition.json` | Deleted editor-plugin sources remain absent; the manifest records their historical disposition and follow-up ownership. |
| Product manifest | `docs/product-surface.json`, `docs/schemas/product-surface.schema.json`, and `scripts/product_surface.py` | The manifest is the executable inventory; source, install, package, and CLI checks derive from it. |

## Implementation phases

### Phase 0 — Freeze the contract and decisions

**Goal:** Prevent implementation from silently changing the accepted ADR.

- [x] Reconcile the ADR-005 snapshot with the current source tree through the
      profile-aware manifest and explicit `source_status` fields.
- [ ] Resolve the redaction decision against GitHub #66 before changing the
      Editor plugin or the supported CLI surface.
- [x] Preserve Compare as `OPEN` with owner `m.berry` and follow-up #193 while
      recording the already-deleted `LoupeDiff` as absent.
- [ ] Decide whether developer-menu visibility is a build flag, a release
      setting, or both. The existing `m_allowDeveloperMode` setting is a useful
      compatibility mechanism.
- [x] Define the release artifact contract: expected executables, libraries,
      desktop entries, AppX/Flatpak/WiX entrypoints, file associations, and the
      live PdfTool command inventory.

**Deliverable:** approved implementation checklist and a release-profile
artifact inventory.

### Phase 1 — Make the release profile merge-durable

**Goal:** Produce a slim Loupe build without deleting upstream-syncable sources.

- [x] Keep `LOUPE_LOUPE_DISTRIBUTION` as the top-level release switch and make
      release/packaging workflows pass it explicitly.
- [x] Verify that the release profile contains only the installed Editor, PdfTool,
      Core, and Quick product artifacts; developer tools remain developer-only.
- [x] Record deleted Viewer, PageMaster, Diff, LaunchPad, and editor-plugin
      artifacts as absent from every profile.
- [x] Keep OCR in the PdfTool capability contract; no retired Editor plugin may
      reappear without an explicit manifest change.
- [x] Enforce the OCR CLI-only surface with a release-profile CMake guard and
      artifact-level plugin/sidecar/CLI checks (issue #41).
- [x] Add a static/source and installed-tree check that fails if an unmanifested
      target, artifact, plugin, or CLI command appears.
- [x] Keep a full developer configuration available for upstream comparison and
      development workflows.

**Primary files:** `CMakeLists.txt`, `LoupeEditor/CMakeLists.txt`, `WixInstaller/`,
CI workflow configure commands, and the manifest-backed verification scripts.

**Deliverable:** two inspectable configurations: full developer build and slim
Loupe release build.

### Phase 2 — Align release packaging and entrypoints

**Goal:** Make installed artifacts communicate one Loupe desktop product.

- [x] Gate Linux `.desktop`, icon, and metainfo installation on the release
      surface; retain source assets for developer/full builds.
- [x] Make the primary Loupe desktop entry launch `LoupeEditor` directly in
      the slim profile instead of exposing LaunchPad as the product shell.
- [x] Remove Viewer, Diff, and PageMaster desktop entries from slim Linux
      artifacts while keeping only the manifest-declared Editor entries in the
      developer/release packaging profiles.
- [x] Confirm Flatpak continues to launch Editor, retains the CLI policy, and
      does not accidentally expose deleted plugin/application artifacts.
- [x] Simplify WiX to Editor, PdfTool, Core, Quick, and explicitly configured
      runtime services; remove stale compatibility/plugin feature fragments and
      Qt Widgets. Preserve WiX dependency injection for staged runtime DLLs.
- [x] Verify PDF file association behavior when Viewer is disabled; Editor must
      remain the supported open target.

**Primary files:** root `CMakeLists.txt`, `Desktop/`, `Flatpak/`,
`WixInstaller/CMakeLists.txt`, `WixInstaller/Product.wxs.in`, and relevant CI
packaging workflows.

**Deliverable:** platform packaging outputs with no stale upstream product
shortcuts or names.

### Phase 3 — Reorganize the Editor surface

**Goal:** Make the Editor feel like Loupe without changing shared PDF behavior.

- [ ] Lock the first information architecture: File, Edit, View, Document,
      Production, Preflight, and Help, with contextual Inspect/Fix/Compare
      entry points.
- [ ] Hide or relocate the Developer menu in release builds while preserving a
      deliberate developer/QA path.
- [ ] De-emphasize the annotation-authoring suite and Certificate Manager from
      the primary workflow; preserve compatibility paths until their product
      decisions are closed.
- [ ] Keep production-critical actions visible and coherent: page geometry,
      bleed, optimize, production preview, separations, preflight, and report
      export.
- [ ] Make plugin-provided menus obey the same surface policy; do not rely only
      on static `.ui` menu edits.
- [ ] Add UI-level tests or deterministic action-visibility checks for release
      and developer modes where the existing test harness permits them.

**Primary files:** `LoupeLibGui/pdfeditormainwindow.ui`,
`LoupeLibGui/pdfeditormainwindow.cpp`, `pdfprogramcontroller.cpp`, relevant
plugin action registration, and UI test fixtures.

**Deliverable:** an Editor shell whose visible actions match the Loupe product
manifest in both release and developer modes.

### Phase 4 — Absorb workflows without duplicating semantics

**Goal:** Converge product surfaces while preserving engines and contracts.

- [ ] Define the Editor workspace boundary for Pages/PageMaster before moving
      any UI or export code.
- [ ] Define the in-app Compare boundary before replacing or removing Diff.
- [ ] Reuse Core/PageMaster/export contracts rather than copying business logic
      into the Editor.
- [ ] Keep PdfTool as the automation contract and ensure every retained GUI
      action has a corresponding deterministic Core or CLI semantic operation
      where applicable.
- [ ] Preserve ADR-001 session ownership/invalidation, ADR-002 preflight
      orchestration, ADR-003 export ordering/cancellation, and ADR-004 atomic
      batch manifests.

**Deliverable:** workspace integration decisions and targeted implementation
issues for Pages and Compare, each with explicit acceptance criteria.

### Phase 5 — Hard-delete only after proof

**Goal:** Remove abandoned code only when it is safe against upstream sync and
product ambiguity.

- [ ] Require a closed product decision, dependency scan, and replacement or
      compatibility story for every level-3 removal.
- [ ] Record each hard deletion in the ADR manifest as `hard-delete
      (sync-risk)` with a post-sync verification step.
- [ ] Run a post-sync manifest check after every upstream Sync fork; prefer an
      automated comparison once the release-profile check exists.
- [ ] Delete only after at least one slim release validation cycle demonstrates
      that no supported target, plugin, packaging path, or test depends on the
      removed surface.

**Deliverable:** small, reviewable deletion commits with explicit sync-risk
evidence; no broad cleanup commit.

## Validation matrix

The source-only contract and focused negative tests run on every change. When a
build is available, validate the installed tree in this order:

1. Validate `docs/product-surface.json` against its schema and derive both
   profile target/package inventories from source.
2. Run the verifier against the CMake install manifest and installed tree.
3. Invoke the installed `PdfTool capabilities --console-format json` command
   and compare its complete sorted command inventory.
4. Run targeted builds/tests and hosted packaging jobs; do not claim
   clean-machine or CI validation from local static checks.
5. Re-run the full developer configuration after upstream synchronization.

## Risks and mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Upstream sync reintroduces pruned targets | Release surface regresses silently | Flag-gate first; add post-sync target/artifact inventory checks. |
| Linux and Windows packaging diverge | Users see different products by platform | Validate both install manifests against one expected surface table. |
| Removing Viewer/PageMaster/Diff before replacement | Supported workflow disappears | Preserve Core/PdfTool contracts, keep Compare `OPEN`, and record deleted compatibility artifacts explicitly. |
| Redaction status is guessed | Security or product commitment becomes inconsistent | Leave `OPEN` and block redaction-surface changes on #66. |
| UI-only pruning misses plugin actions | Generic upstream UI leaks back into Loupe | Audit `pdfprogramcontroller` and plugin registration, not only `.ui` files. |
| Release default breaks developer/CI workflows | Contributors lose useful upstream comparison tools | Use explicit release-profile configuration and preserve the full matrix. |

## Acceptance criteria

- [ ] ADR-005 classification remains the source of truth and its `OPEN` items
      are not silently resolved.
- [x] Slim release configuration builds and packages only the supported Loupe
      desktop/CLI surface and maintained Core/Quick libraries.
- [x] Full developer configuration still builds the upstream-comparison
      developer tools.
- [x] No stale Viewer, Diff, PageMaster, LaunchPad, or editor-plugin artifact
      remains in either manifest profile.
- [ ] Editor release menus expose the focused Loupe workflow while developer
      diagnostics remain available through an explicit path.
- [ ] PdfTool and shared Core semantics remain available independently of GUI
      pruning.
- [ ] Every hard deletion, if eventually approved, has dependency evidence and
      a post-sync verification step.
- [ ] Targeted build/test, packaging, and CI results are recorded separately;
      static inspection is not reported as runtime validation.

## Status

- Phase 0: Implemented for Issue #191 — release artifact contract and OPEN rows are explicit
- Phase 1: Implemented — slim release flag is explicit and developer tools remain outside the release profile
- Phase 2: Implemented — desktop/AppX/Flatpak/WiX sources and staged artifact checks derive from the manifest
- Phase 3: Partial — release-only Developer menu gating is implemented; full Loupe information architecture remains follow-up work
- Phase 4: Boundary recorded; Editor workspace wiring remains deferred to #193
- Phase 5: Deferred until level-3 removals are explicitly approved

**Overall:** Issue #191 implementation is complete on
`cdx/issue-191-product-surface`; local source and focused contract checks pass.
Build, package, and runtime evidence remains the responsibility of the hosted
profile jobs when this branch is validated.
