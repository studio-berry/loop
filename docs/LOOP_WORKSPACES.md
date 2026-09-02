# Loop workspace boundaries

This document is the workspace companion to the authoritative product-surface
contract in issue #191. It describes where an operator finds a capability; it
does not add a second application shell or move UI code. Editor workspace wiring
is deferred to #193 and remains outside the pre-0.1.1 GUI scope.

## Product surfaces

Loop has two product surfaces:

- **Loop** — `LoopEditor`, the interactive desktop shell. Opening a PDF is
  the Document workspace and includes the standard document-viewing behavior.
- **Loop CLI** — `PdfTool`, the headless and automation surface. Its command
  names, JSON envelopes, and machine-readable capability discovery remain the
  automation contract.
- `LoopLibCore` and `LoopLibQuick` are maintained implementation libraries,
  not additional user-facing products.

Former standalone applications and editor-plugin artifacts are deleted and
absent from both supported profiles. Their dispositions remain in
`docs/product-surface.json` so an accidental upstream reintroduction fails
verification.

Developer and qualification tools are not product surfaces. They do not
receive a separate Loop desktop entry, AppX application, or product identity.

## Workspace boundaries

| Workspace | Owns | Drives | Explicitly does not own |
| --- | --- | --- | --- |
| Document | Open, view, navigate, save/export, and ordinary PDF interaction | `LoopEditor`, `LoopLibQuick`, shared document/session contracts | A second interactive document product or a second document model |
| Preflight | Run/rerun/cancel inspection, findings, evidence, report export, and stale-result state | Core `PreflightEngine`, `PdfTool preflight`, Quick shell contract | A GUI-only interpretation of the CLI report |
| Production Preview | Soft proofing, output preview, separations, and production rendering evidence | `LoopLibCore`, `LoopLibQuick`, shared render/color contracts | Final approval or an alternate PDF-writing pipeline |
| Pages / Production | Multi-document assembly, page geometry, crop, regrouping, bleed, optimization, and export | `PDFPageMasterExport`, ADR-003 stage order, ADR-004 batch manifest | A copied page-production engine or a reordered export pipeline |
| Inspect | Contextual page, image, object, dimension, color, and evidence inspection | Core inspection APIs and the Quick shell contract | A standalone inspector application |
| Fix | Deterministic, bounded corrective operations with preview, approval, output, and revalidation | Core repair operations and `PdfTool repair` | Silent mutation, GUI-only business logic, or implicit approval |
| Compare | Proposed PDF comparison and production-proof evidence | Core `PDFDiff` contract if the product boundary is approved | An automatic replacement of the retired comparison product |

The shell issue (#193) may model these as stateful workspaces, but switching
workspace must preserve the open document and preflight revision. A workspace
is not a new executable and must not own a duplicate Core semantic path.

## Page-production disposition and capability crosswalk

Page production is recorded as **CLI-ONLY** and its former source is deleted. Its
historical UI action inventory maps to the Pages / Production workspace later,
while `PDFPageMasterExport` and its ADR-003/ADR-004 contracts remain the single
source of truth. The retained capability inventory is assigned a destination or
an explicit compatibility disposition. The following action map is retained as
product intent; it does not imply that a standalone executable or UI file is
still shipped.

| Page-production action IDs | Disposition | Destination / contract |
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
| `actionGet_Source`, `actionBecomeASponsor`, `actionAbout`, `actionPrepare_Icon_Theme` | KEEP / ADVANCED | Loop Help or developer/compatibility path; not a production capability |

No page-production semantic contract is silently retired, but the standalone
executable is absent from both profiles. Export order remains the ADR-003
contract: assembly, preflight, page geometry,
bleed/content fixups, image optimization, then write, with ADR-004 manifest and
rollback behavior unchanged.

## Compare disposition

Compare is **OPEN**, not implicitly absorbed. The Core `PDFDiff` contract is
retained while the standalone comparison executable is absent from both profiles
because its source was already deleted. The owner is `m.berry`; #193 is the follow-up for the shell
boundary and #197 is the release exit gate. No new UI replacement or product
commitment is authorized by this document.

## Packaging rules

The checked-in `docs/product-surface.json` manifest is the source of truth for
surface disposition and the expected developer/release packaging inventory.
`scripts/verify_product_surface.py` (with
`scripts/verify-loop-surface.ps1` as the Windows/PowerShell entry point)
consumes that manifest. The release profile
must have:

- one Linux desktop entry, `io.github.mberrys.Loop-pdf.desktop`, launching
  `LoopEditor` with `application/pdf` association;
- one AppX application, `LoopEditor`, with the same PDF association;
- no retired product desktop/AppX entry; and
- only the manifest-declared `LoopEditor`, `PdfTool`, `LoopLibCore`, and
  `LoopLibQuick` first-party artifacts; deleted compatibility/plugin artifacts
  are forbidden.

The full developer profile adds the explicit Editor desktop entry for developer
testing and builds the three declared developer tools. Developer-only tools
remain opt-in build targets and are not packaging surfaces.
