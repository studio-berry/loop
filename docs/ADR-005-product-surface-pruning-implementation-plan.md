# ADR-005 Implementation Plan: Product Surface Pruning

**Branch:** `product-pruning`
**Source decision:** [ADR-005: Product surface pruning classification](https://app.notion.com/p/3b69cb079ddb813492a8cb6f84f23d14?pvs=204)
**Status:** 0.2B transitional packaging implemented; #192 establishes the
workspace boundary and authoritative product-surface contract; Editor workspace
integration remains planned for #193 / 0.0.1B.

## 0.2B transitional surface contract

The release package presents two product surfaces:

- **Loupe** is the `Pdf4QtEditor` desktop shell. Opening a PDF provides the
  normal viewing behavior; future Editor workspaces are named Pages /
  Production, Compare, and Production Preview.
- **Loupe CLI** is the product-facing name for `PdfTool`. The `PdfTool`
  executable, command names, machine-readable contracts, and automation
  compatibility remain unchanged.

The release profile keeps the following targets buildable and installed for
compatibility/direct invocation: `Pdf4QtViewer`, `Pdf4QtPageMaster`,
`Pdf4QtDiff`, and `Pdf4QtLaunchPad`. CodeGenerator, JBIG2 Viewer,
PdfExampleGenerator, and the Scanner plugin remain retained by the release
build profile. None of these compatibility surfaces receives a separate Loupe
desktop or AppX product entry.

The Editor-facing Pages / Production and Compare seams must reuse the existing
Core/PageMaster export and Core Diff contracts. This 0.2B slice does not move
their visible UI into Editor; that workspace integration is a 0.3 deliverable.

Issue #192 records the boundary in [LOUPE_WORKSPACES.md](LOUPE_WORKSPACES.md)
and the profile-aware inventory in [product-surface.json](product-surface.json),
validated by `scripts/verify-loupe-surface.ps1`. The manifest is the executable
inventory for developer and `loupe-release` packaging profiles; this plan and
ADR-005 remain the decision record.

## Objective

Move Loupe from an upstream-shaped multi-application distribution to a focused
print-production product surface while preserving shared PDF4QT engine code,
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
- Release-profile gating for Viewer, Diff, PageMaster, LaunchPad, developer
  utilities, and non-primary plugins.
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
| Distribution switch | `CMakeLists.txt:68-107` | Reuse `PDF4QT_LOUPE_DISTRIBUTION`; do not introduce per-surface flags until a real exception needs one. |
| Conditional application targets | `CMakeLists.txt:250-274` | Keep source directories and gate targets through the release profile. |
| Linux install surface | `CMakeLists.txt:344-368` | Make `.desktop`, icon, and metainfo installation follow the same product profile as binaries. |
| Windows packaging | `WixInstaller/CMakeLists.txt` and `WixInstaller/Product.wxs.in` | Preserve conditional Viewer/PageMaster/Diff features; verify file associations resolve to Loupe when those features are absent. |
| Editor menus | `Pdf4QtLibGui/pdfeditormainwindow.ui` and `pdfeditormainwindow.cpp` | Reorganize or hide inherited menus through the Editor shell; avoid deleting action implementations prematurely. |
| Plugin registry/build | `Pdf4QtEditorPlugins/CMakeLists.txt` and `pdfprogramcontroller.cpp` | Separate retained production plugins from optional/deferred plugins without changing shared action contracts. |
| LaunchPad | `Pdf4QtLaunchPad/` and `Desktop/io.github.mberrys.Loupe-pdf.desktop` | Compatibility target remains available for direct invocation; the release desktop entry launches Editor, and the developer profile may retain compatibility entries. |
| Product manifest | `docs/product-surface.json` and `docs/schemas/product-surface.schema.json` | The profile-aware manifest is the executable inventory; ADR-005 remains the decision source and implementation status links both records. |

## Implementation phases

### Phase 0 — Freeze the contract and decisions

**Goal:** Prevent implementation from silently changing the accepted ADR.

- [ ] Keep the ADR-005 manifest unchanged except for explicit status/evidence
      updates.
- [ ] Resolve the redaction decision against GitHub #66 before changing the
      Editor plugin or the supported CLI surface.
- [ ] Decide whether Compare is a supported Loupe workspace or remains
      deferred; do not remove `Pdf4QtDiff` while this is `OPEN`.
- [ ] Decide whether developer-menu visibility is a build flag, a release
      setting, or both. The existing `m_allowDeveloperMode` setting is a useful
      compatibility mechanism.
- [ ] Define the release artifact contract: expected executables, plugins,
      desktop entries, file associations, and CLI commands for each supported
      platform.

**Deliverable:** approved implementation checklist and a release-profile
artifact inventory.

### Phase 1 — Make the release profile merge-durable

**Goal:** Produce a slim Loupe build without deleting upstream-syncable sources.

- [ ] Keep `PDF4QT_LOUPE_DISTRIBUTION` as the top-level release switch and make
      release/packaging workflows pass it explicitly rather than relying on a
      developer default.
- [ ] Verify that the profile disables Viewer, PageMaster, Diff, LaunchPad,
      CodeGenerator, JBIG2 viewer, ExampleGenerator, AudioBook, and Scanner,
      while retaining Editor, PdfTool, Core libraries, LoupePreflight, and the
      production inspection plugins.
- [ ] Keep OCR explicitly CLI-only in release packaging; do not disable
      `PdfTool ocr` when disabling the Editor OCR plugin.
- [x] Enforce the OCR CLI-only surface with a release-profile CMake guard and
      artifact-level plugin/sidecar/CLI checks (issue #41).
- [ ] Add a static release-profile check that fails if a pruned target or
      plugin is built, installed, or packaged unexpectedly.
- [ ] Keep a full developer configuration available for upstream comparison and
      development workflows.

**Primary files:** `CMakeLists.txt`, `Pdf4QtEditorPlugins/CMakeLists.txt`, CI
workflow configure commands, and a new narrow packaging-surface verification
script if existing checks cannot express the inventory.

**Deliverable:** two inspectable configurations: full developer build and slim
Loupe release build.

### Phase 2 — Align release packaging and entrypoints

**Goal:** Make installed artifacts communicate one Loupe desktop product.

- [ ] Gate Linux `.desktop`, icon, and metainfo installation on the release
      surface; retain source assets for developer/full builds.
- [ ] Make the primary Loupe desktop entry launch `Pdf4QtEditor` directly in
      the slim profile instead of exposing LaunchPad as the product shell.
- [ ] Remove Viewer, Diff, and PageMaster desktop entries from slim Linux
      artifacts while retaining them in the full developer profile.
- [ ] Confirm Flatpak continues to launch Editor, retains the CLI policy, and
      does not accidentally expose disabled plugin/application artifacts.
- [ ] Preserve WiX conditional feature generation, then verify that slim
      installers contain Editor, PdfTool, and retained plugins only.
- [ ] Verify PDF file association behavior when Viewer is disabled; Editor must
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

**Primary files:** `Pdf4QtLibGui/pdfeditormainwindow.ui`,
`Pdf4QtLibGui/pdfeditormainwindow.cpp`, `pdfprogramcontroller.cpp`, relevant
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

Do not run builds or reconfigure CMake as part of the planning pass. When
implementation begins, validate in this order:

1. Static configuration review: full versus slim option values and target
   graph.
2. Target/build inventory: expected executables and plugin libraries for each
   profile.
3. Install/package inventory: Linux desktop entries/icons, Flatpak contents,
   and WiX feature/file-association behavior.
4. Editor action inventory: release-visible versus developer-visible menus and
   plugin actions.
5. Targeted build of touched targets, then relevant Qt tests; do not claim
   clean-machine, installer, or CI validation from local static checks.
6. Full developer configuration and upstream-sync comparison after slim-profile
   changes.

## Risks and mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Upstream sync reintroduces pruned targets | Release surface regresses silently | Flag-gate first; add post-sync target/artifact inventory checks. |
| Linux and Windows packaging diverge | Users see different products by platform | Validate both install manifests against one expected surface table. |
| Removing Viewer/PageMaster/Diff before replacement | Supported workflow disappears | Keep source and developer builds; require workspace/Compare decisions first. |
| Redaction status is guessed | Security or product commitment becomes inconsistent | Leave `OPEN` and block redaction-surface changes on #66. |
| UI-only pruning misses plugin actions | Generic upstream UI leaks back into Loupe | Audit `pdfprogramcontroller` and plugin registration, not only `.ui` files. |
| Release default breaks developer/CI workflows | Contributors lose useful upstream comparison tools | Use explicit release-profile configuration and preserve the full matrix. |

## Acceptance criteria

- [ ] ADR-005 classification remains the source of truth and its `OPEN` items
      are not silently resolved.
- [ ] Slim release configuration builds and packages only the supported Loupe
      desktop/CLI surface and retained production plugins.
- [ ] Full developer configuration still builds the upstream-comparison
      surfaces.
- [ ] No stale Viewer, Diff, PageMaster, or LaunchPad entry remains in slim
      release artifacts.
- [ ] Editor release menus expose the focused Loupe workflow while developer
      diagnostics remain available through an explicit path.
- [ ] PdfTool and shared Core semantics remain available independently of GUI
      pruning.
- [ ] Every hard deletion, if eventually approved, has dependency evidence and
      a post-sync verification step.
- [ ] Targeted build/test, packaging, and CI results are recorded separately;
      static inspection is not reported as runtime validation.

## Status

- Phase 0: Partial — release artifact contract implemented; OPEN product decisions remain
- Phase 1: Implemented — slim release flag is explicit in release CI/Flatpak and inherited targets remain source-preserved
- Phase 2: Implemented — primary desktop/AppX surfaces and staged artifact checks are aligned; runtime packaging validation pending
- Phase 3: Partial — release-only Developer menu gating is implemented; full Loupe information architecture remains follow-up work
- Phase 4: Boundary recorded; Editor workspace wiring remains deferred to #193
- Phase 5: Deferred until level-3 removals are explicitly approved

**Overall:** Initial implementation slice complete on `product-pruning`; build,
package, and runtime validation are pending an explicit build/test pass.
