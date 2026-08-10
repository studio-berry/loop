# Loupe workspace boundaries

This document is the product boundary for issue #192. It describes where an
operator finds a capability; it does not add a second application shell or
move UI code. Editor workspace wiring is deferred to #193 and remains outside
the pre-0.0.3 GUI scope.

## Product surfaces

Loupe has two product surfaces:

- **Loupe** — `Pdf4QtEditor`, the interactive desktop shell. Opening a PDF is
  the Document workspace and includes the inherited Viewer behavior.
- **Loupe CLI** — `PdfTool`, the headless and automation surface. Its command
  names, JSON envelopes, and machine-readable capability discovery remain the
  automation contract.

Compatibility executables may remain buildable and directly invokable. They do
not receive a separate Loupe desktop entry, AppX application, or product
identity in the release profile.

## Workspace boundaries

| Workspace | Owns | Drives | Explicitly does not own |
| --- | --- | --- | --- |
| Document | Open, view, navigate, save/export, and ordinary PDF interaction | `Pdf4QtEditor`, `Pdf4QtLibGui`, shared document/session contracts | A separate Viewer product or a second document model |
| Preflight | Run/rerun/cancel inspection, findings, evidence, report export, and stale-result state | Core `PreflightEngine`, `PdfTool preflight`, `LoupePreflightPlugin` | A GUI-only interpretation of the CLI report |
| Production Preview | Soft proofing, output preview, separations, and production rendering evidence | `OutputPreviewPlugin`, `SoftProofingPlugin`, shared render/color contracts | Final approval or an alternate PDF-writing pipeline |
| Pages / Production | Multi-document assembly, page geometry, crop, regrouping, bleed, optimization, and export | `PDFPageMasterExport`, ADR-003 stage order, ADR-004 batch manifest | A copied PageMaster engine or a reordered export pipeline |
| Inspect | Contextual page, image, object, dimension, color, and evidence inspection | `DimensionsPlugin`, `ObjectInspectorPlugin`, Core inspection APIs | A standalone inspector application |
| Fix | Deterministic, bounded corrective operations with preview, approval, output, and revalidation | Core repair operations and `PdfTool repair` | Silent mutation, GUI-only business logic, or implicit approval |
| Compare | Proposed PDF comparison and production-proof evidence | Core `PDFDiff` contract if the product boundary is approved | An automatic replacement of `Pdf4QtDiff` while the decision is `OPEN` |

The shell issue (#193) may model these as stateful workspaces, but switching
workspace must preserve the open document and preflight revision. A workspace
is not a new executable and must not own a duplicate Core semantic path.

## PageMaster disposition and capability crosswalk

`Pdf4QtPageMaster` is **ABSORB**: its UI becomes the Pages / Production
workspace later, while `PDFPageMasterExport` and its ADR-003/ADR-004 contracts
remain the single source of truth. The following is the complete action
inventory from `Pdf4QtPageMaster/mainwindow.ui`; every action is assigned a
destination or an explicit compatibility disposition.

| PageMaster action IDs | Disposition | Destination / contract |
| --- | --- | --- |
| `actionOpenWorkspace`, `actionSaveWorkspace`, `actionAddDocuments`, `actionSaveCheckpoint`, `actionLoadCheckpoint`, `actionClear`, `actionClose`, `actionClearRecent`, `actionClearSearch` | ABSORB | Pages / Production workspace lifecycle, search/filter reset, and ADR-004 checkpoint/manifest behavior |
| `actionCloneSelection`, `actionRemoveSelection`, `actionReplaceSelection`, `actionRestoreRemovedItems`, `actionCut`, `actionCopy`, `actionPaste` | ABSORB | Pages / Production document-item editing over the existing page-item model |
| `actionInsert_PDF`, `actionInsertPDFPages`, `actionInsert_Image`, `actionInsert_Empty_Page` | ABSORB | Pages / Production assembly stage in `PDFPageMasterExport` |
| `actionPageGeometry`, `actionRotate_Left`, `actionRotate_Right`, `actionResetRotation`, `actionCropPages`, `actionProperties` | ABSORB | Pages / Production geometry stage; preserve box and rotation semantics |
| `actionGroup`, `actionUngroup`, `actionRenameGroup` | ABSORB | Pages / Production grouping model |
| `actionSelect_None`, `actionSelect_All`, `actionSelectPageRange`, `actionSelect_Even`, `actionSelect_Odd`, `actionSelect_Portrait`, `actionSelect_Landscape`, `actionSelectVisible`, `actionInvert_Selection` | ABSORB | Pages / Production selection model |
| `actionSortByFileName`, `actionSortBySource`, `actionSortByPageNumber`, `actionSortByType`, `actionReverseOrder` | ABSORB | Pages / Production ordering model |
| `actionZoom_In`, `actionZoom_Out`, `actionShow_Document_Title_in_Items`, `actionShowDetailsView` | ABSORB | Pages / Production presentation state; no export semantics change |
| `actionUnited_Document`, `actionSeparate_to_Multiple_Documents`, `actionSeparate_to_Multiple_Documents_Grouped`, `actionSplit` | ABSORB | Pages / Production make/export operations and existing CLI equivalents |
| `actionRegroup_Even_Odd`, `actionRegroup_by_Page_Pairs`, `actionRegroup_by_Outline`, `actionRegroup_by_Alternating_Pages`, `actionRegroup_by_Alternating_Pages_Reversed_Order`, `actionRegroup_Reverse` | ABSORB | Pages / Production regroup operations |
| `actionUndo`, `actionRedo` | ABSORB | Pages / Production history; must remain scoped to the workspace document model |
| `actionGet_Source`, `actionBecomeASponsor`, `actionAbout`, `actionPrepare_Icon_Theme` | KEEP / ADVANCED | Loupe Help or developer/compatibility path; not a production capability |

No PageMaster action is silently retired. The standalone executable remains a
compatibility surface until #193 provides the shell host and parity evidence.
Export order remains the ADR-003 contract: assembly, preflight, page geometry,
bleed/content fixups, image optimization, then write, with ADR-004 manifest and
rollback behavior unchanged.

## Compare disposition

Compare is **OPEN**, not implicitly absorbed. The Core `PDFDiff` contract is
retained and `Pdf4QtDiff` remains directly invokable in both developer and
release compatibility inventories, but the release package has no Diff desktop
entry or AppX product entry. The owner is `m.berry`; #193 is the follow-up for
the shell boundary and #197 is the release exit gate. No deletion or UI
replacement is authorized by this document.

## Packaging rules

The checked-in `docs/product-surface.json` manifest is the source of truth for
surface disposition and the expected developer/release packaging inventory.
`scripts/verify-loupe-surface.ps1` consumes that manifest. The release profile
must have:

- one Linux desktop entry, `io.github.mberrys.Loupe-pdf.desktop`, launching
  `Pdf4QtEditor` with `application/pdf` association;
- one AppX application, `Pdf4QtEditor`, with the same PDF association;
- no Viewer, PageMaster, Diff, or LaunchPad desktop/AppX entry; and
- the retained compatibility binaries/plugins listed by the manifest where
  direct invocation or release-profile policy requires them.

The full developer profile keeps the inherited desktop entries for comparison
and direct testing. This is packaging visibility, not a license to expose
multiple Loupe products to release users.
