# ADR-005: Product surface pruning classification (0.1.1A)

**Status:** accepted
**Date:** 2026-08-07
**Deciders:** Loupe 0.1.1 milestone (Notion: Product Convergence); 1.1A audit

## Context

Loupe is a PDF4QT fork. The inherited codebase still presents PDF4QT's full
generic-PDF-toolkit product surface (multiple desktop executables, upstream
menu structure, optional plugins, and inconsistent packaging). Milestone **0.1.1**
starts with **1.1A — Product pruning**: decide what Loupe is before redesigning
the shell (1.1B) or expanding preflight GUI work (1.1C+).

The pruning test (from Notion):

> Does this capability help someone inspect, prepare, prove, manipulate, or
> deliver a PDF?

Upstream sync happens via on-demand GitHub **Sync fork** (`AGENTS.md`). Deleted
files or removed CMake targets can reappear on merge if upstream still ships
them. Pruning mechanics must therefore distinguish merge-durable flag-gating from
merge-fragile hard deletion.

`AGENTS.md` still documents a split-surface architecture that conflicts with
Notion 1.1B (Viewer eliminated, Diff → Compare, PageMaster → in-app workspace).
That conflict is recorded in the manifest; this ADR does not rewrite `AGENTS.md`.

## Decision

Implementation note (2026-08-09): issue #192 makes the profile-aware
machine-readable inventory in `docs/product-surface.json` the executable
packaging contract. The table below remains the decision snapshot; current
artifact evidence is maintained in that manifest and checked by
`scripts/verify-loupe-surface.ps1`.

- **Deliverable:** the product-surface manifest — the authoritative inventory and
classification for executables, editor plugins, core menu groups, and packaging
surfaces — is **embedded in this ADR** (the manifest tables below). **1.1A
produces classification only** — no UI removal, build default changes, or code
deletion in this pass.
- **Per-surface recommendation:** every classified surface carries an explicit
**Recommendation (Prune / Retain)** verdict (second column of each manifest
table), derived from the Notion disposition and the recommended pruning level,
so the manifest is directly actionable rather than only leveled.
- **Three pruning levels** (least → most committing), recorded per surface in the
manifest:
    1. Remove from Loupe UI.
    2. Stop building/shipping the executable or plugin.
    3. Remove code/dependency entirely.
- **Classification columns** in the manifest: recommendation, current path,
PDF4QT origin (inherited/modified), Notion disposition (verbatim from the
Product Convergence doc), target Loupe surface, recommended pruning level, sync
durability, timing, evidence, and open questions.
- **Sync durability default:** pruning that stops shipping a surface should
**default to flag-gating** (e.g. existing `PDF4QT_LOUPE_DISTRIBUTION`,
`PDF4QT_PLUGIN_OCR`, `PDF4QT_PLUGIN_AUDIOBOOK`, `PDF4QT_PLUGIN_SCANNER`, or
new per-surface flags) unless the surface is being **fully abandoned** and no
future upstream fixes are wanted. In the full-abandonment case,
**hard-delete** is acceptable but must be logged in the manifest as
**hard-delete (sync-risk)** and tracked for a **post-sync check** after every
future upstream Sync fork (or automated manifest/CI cross-check when added).
- **Unresolved dispositions** use a searchable **`OPEN`** token in the manifest
(minimum: redaction / GitHub #66). No guessing on blocked items.
- **Fork policy alignment (recorded, not implemented here):** upstream PDF4QT is
the engine and selective fix source; Loupe does not preserve upstream
application UX parity. Pruning is an intentional product decision, not fork
debt.

## Consequences

- **No build, packaging, or UI behavior changes ship in 0.1.1A.** Engineers and
1.1B planning consume the manifest as the backlog input.
- 1.1B should prefer level-1 (UI hide/reorganize) and level-2 (flag-gated stop
shipping) before level-3 hard deletes.
- Any future level-3 removal requires an explicit sync-risk entry and a defined
post-sync verification step; otherwise reintroduced upstream files may ship
silently.
- Redaction remains **OPEN** until #66 resolves whether redaction stays a
supported Loupe/CLI capability.
- `AGENTS.md` architecture guidance remains unchanged in 1.1A; a one-line
"see also" pointer to the manifest is optional and does not alter the existing
table.

---

## Editor plugins

Eleven plugin subdirectories per `Pdf4QtEditorPlugins/CMakeLists.txt` (AudioBook
and Scanner gated by `PDF4QT_PLUGIN_*`; Ocr by `PDF4QT_PLUGIN_OCR`).

| Surface | Recommendation (Prune / Retain) | Current location (path) | PDF4QT origin | Notion disposition | Target Loupe surface | Pruning level recommended | Sync durability | Timing | Evidence | Open questions |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| LoupePreflightPlugin | **RETAIN (elevate)** | `Pdf4QtEditorPlugins/LoupePreflightPlugin/` | modified (Loupe) | Preflight CLI — **CORE**, expose through GUI | Preflight (first-class GUI in 1.1C) | N-A | N/A no action | defer to 1.1C | Always built `Pdf4QtEditorPlugins/CMakeLists.txt:33`; WiX `cmpLoupePreflightPlugin` (`Product.wxs.in:306`) | In-process vs. QProcess for V1 GUI? |
| OutputPreviewPlugin | **RETAIN (elevate)** | `Pdf4QtEditorPlugins/OutputPreviewPlugin/` | inherited | Output Preview — **CORE — elevate significantly** | Production Preview | 1 | N/A no action | defer to 1.1B / 1.3 | Always built `CMakeLists.txt:28`; WiX `cmpOutputPreviewPlugin` (`Product.wxs.in:291`) | Merge with SoftProofing/separations in new shell? |
| SoftProofingPlugin | **RETAIN (elevate)** | `Pdf4QtEditorPlugins/SoftProofingPlugin/` | inherited | Separations — **CORE — elevate** | Production Preview / separations | 1 | N/A no action | defer to 1.1B | Always built `CMakeLists.txt:31`; WiX `cmpSoftProofingPlugin` (`Product.wxs.in:297`) |  |
| DimensionsPlugin | **RETAIN (elevate)** | `Pdf4QtEditorPlugins/DimensionsPlugin/` | inherited | Page boxes / geometry — **CORE — elevate** | Pages / Inspect (page geometry) | 1 | N/A no action | defer to 1.1B | Always built `CMakeLists.txt:26`; WiX `cmpDimensionsPlugin` (`Product.wxs.in:285`) |  |
| ObjectInspectorPlugin | **RETAIN (advanced)** | `Pdf4QtEditorPlugins/ObjectInspectorPlugin/` | inherited | General PDF object inspection — **KEEP as advanced** | Inspect (advanced) | 1 | N/A no action | defer to 1.1B | Always built `CMakeLists.txt:27`; WiX `cmpObjectInspectorPlugin` (`Product.wxs.in:288`) |  |
| EditorPlugin | **RETAIN (partial)** | `Pdf4QtEditorPlugins/EditorPlugin/` | inherited | Generic PDF editing — Keep only useful primitives | Fix / selective editing primitives | 1 | N/A no action | defer later | Always built `CMakeLists.txt:32`; WiX `cmpEditorPlugin` (`Product.wxs.in:303`) | Which primitives survive toolbar pruning? |
| SignaturePlugin | **RETAIN (deferred / advanced)** | `Pdf4QtEditorPlugins/SignaturePlugin/` | inherited | Digital signing — Candidate for later/advanced mode | Advanced / later mode | 1 | N/A no action | defer later | Always built `CMakeLists.txt:30`; WiX `cmpSignaturePlugin` (`Product.wxs.in:300`) |  |
| RedactPlugin | **OPEN** — TBD vs. CLI `redact` | `Pdf4QtEditorPlugins/RedactPlugin/` | inherited | Redaction — Probably outside primary print workflow | **OPEN** — TBD vs. CLI `redact` | 1 (UI) / 2 (plugin ship) | flag-gated (new `PDF4QT_PLUGIN_REDACT` recommended; not present today) | **blocked** on #66 | Always built `CMakeLists.txt:29`; WiX `cmpRedactPlugin` (`Product.wxs.in:294`); PdfTool `redact` command; active hardening in `docs/BUG_HUNT_2026-08-04.md` / `UnitTestsRedactVerifier` | **OPEN** — retain & harden vs. remove from supported product surface (#66) |
| OcrPlugin | **PRUNE from UI / RETAIN as CLI** | `Pdf4QtEditorPlugins/OcrPlugin/` | modified (Loupe) | *(V1 policy: CLI-only OCR — MIC-343)* | removed from primary UI; CLI-only | 2 | N/A already flag-gated (`PDF4QT_PLUGIN_OCR`; Flatpak sets OFF) | defer later | Gated `CMakeLists.txt:34-36`; root `CMakeLists.txt:102-107` MIC-343 comment; WiX optional `${PDF4QT_WIX_PLUGIN_OCR}` (`Product.wxs.in:309`); Flatpak `config-opts` `-DPDF4QT_PLUGIN_OCR=OFF` (`io.github.mberrys.Loupe-pdf.json:131`) |  |
| AudioBookPlugin | **PRUNE** | `Pdf4QtEditorPlugins/AudioBookPlugin/` | inherited | *(not in disposition table — accessibility TTS)* | removed / non-primary | 2 | N/A already flag-gated (`PDF4QT_PLUGIN_AUDIOBOOK` / `PDF4QT_LOUPE_DISTRIBUTION`) | defer to 1.1B | Gated `CMakeLists.txt:23-25`; WiX optional `${PDF4QT_WIX_PLUGIN_AUDIOBOOK}` (`Product.wxs.in:284`) | Any accessibility obligation to retain TTS? |
| ScannerPlugin | **PRUNE** | `Pdf4QtEditorPlugins/ScannerPlugin/` | inherited | *(not in disposition table — scan-to-PDF)* | removed | 2 | N/A already flag-gated (`PDF4QT_PLUGIN_SCANNER` / `PDF4QT_LOUPE_DISTRIBUTION`) | defer to 1.1B | Gated `CMakeLists.txt:37-39`; WiX optional `${PDF4QT_WIX_PLUGIN_SCANNER}` (`Product.wxs.in:310`) |  |

**RESOLVED:** No standalone Forms, Certificates, or Attachments **plugins** exist
(`grep` across `Pdf4QtEditorPlugins/` — only the 11 directories above).

---

## Editor core menu groups

Menu structure from `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuFile`,
`menuEdit`, `menuView`, `menuInsert`, `menuGoTo`, `menuTools`, `menuHelp`,
`menuDeveloper`). All non-separator `action*` entries map to a row below (cluster
or named group).

| Surface | Recommendation (Prune / Retain) | Current location (path) | PDF4QT origin | Notion disposition | Target Loupe surface | Pruning level recommended | Sync durability | Timing | Evidence | Open questions |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| File menu | **RETAIN (reorganize)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuFile`) | inherited | PDF rendering/viewing — **CORE — keep** | File (open/save/export/print) | 1 (reorganize under Loupe IA) | N/A no action | defer to 1.1B | Actions: Open, Close, Auto-refresh, Save, Save As, Send by E-Mail, Print, Render to Images, Properties, Clear recent, Quit (`ui` lines 30-45) | Which items move to Production vs. File in new menu model? |
| View menu | **RETAIN (redesign)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuView`) | inherited | Page navigation / zoom — **CORE — keep, redesign** | Document (navigate/zoom/view modes) | 1 | N/A no action | defer to 1.1B | Page layout, rendering options, rotate, zoom, fit, color modes (`ui` lines 79-116) |  |
| Go To menu | **RETAIN (redesign)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuGoTo`) | inherited | Page navigation / zoom — **CORE — keep, redesign** | Document / Pages rail | 1 | N/A no action | defer to 1.1B | Navigation, bookmarks, bookmark settings (`ui` lines 55-69) |  |
| Edit — Encryption / Optimize / Sanitize / Page Geometry / Bleed Fixup / Bitonal cluster | **RETAIN (elevate)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuEdit`) | inherited + Loupe | Encryption/security — Keep essential, simplify UI; Compression/optimization — **KEEP**; Page boxes / geometry — **CORE — elevate** | Production / Fix | 1 | N/A no action | defer to 1.1B | `actionEncryption`, `actionOptimize`, `actionOptimizeImages`, `actionSanitize`, `actionRemoveExternalLinks`, `actionPageGeometry`, `actionBleedFixup`, `actionCreateBitonalDocument` (`ui` lines 166-174); wired in `pdfeditormainwindow.cpp` | Bleed fixup vs. PageMaster batch — surface order? |
| Edit — text selection / find (non-cluster) | **RETAIN** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuEdit`) | inherited | Text/object inspection — **KEEP** | Document / Inspect | 1 | N/A no action | defer to 1.1B | Undo/redo, find, select/copy text, select table (`ui` lines 152-164) |  |
| Insert — annotation-authoring suite | **PRUNE (de-emphasize)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuInsert`) | inherited | Annotation-authoring suite — **Likely remove/de-emphasize**; Basic annotations — **Probably reduce** | Contextual review markup only (if any) | 1 | N/A no action | defer to 1.1B | Sticky notes, text highlights, hyperlinks, inline text, lines/shapes, stamps, page numbers (`ui` lines 184-233; submenus `menuStamp`, `menuTextHighlight`, `menuHyperlinkToThisPDF`) | Any markup required for prepress review workflows? |
| Tools — inspection utilities | **RETAIN** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuTools`) | inherited | Text/object inspection — **KEEP**; Image extraction — CLI/advanced | Inspect / advanced | 1 | N/A no action | defer to 1.1B | Magnifier, Screenshot, Extract Image, Rendering Errors, Options, Reset (`ui` lines 122-129) | Screenshot/extract-image → CLI-only per Notion? |
| Tools — Certificate Manager | **PRUNE (hide)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuTools`) | inherited | Certificates UI — **De-emphasize/remove unless required**; Certificate management — Probably unnecessary product surface | removed or advanced/hidden | 1 | N/A no action | defer to 1.1B | `actionCertificateManager` (`ui` line 131); wired `pdfeditormainwindow.cpp:190` | **RESOLVED** — not a plugin; menu action only. Required for signature verification workflows? |
| Developer menu | **PRUNE (dev builds only)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuDeveloper`) | inherited | Generic developer/debug utilities — Remove from user-facing application | removed (dev builds only) | 1 | N/A no action | defer to 1.1B | `actionShow_Text_Blocks`, `actionShow_Text_Lines` only (`ui` lines 145-146) | Gate behind `PDF4QT_DEV_MENU` or compile-time flag? |
| Help menu | **RETAIN (rebrand)** | `Pdf4QtLibGui/pdfeditormainwindow.ui` (`menuHelp`) | inherited | *(upstream sponsor links)* | Help (Loupe-branded) | 1 | N/A no action | defer to 1.1B | Get Source, Become a Sponsor, About (`ui` lines 137-139) | Remove upstream sponsor CTAs? |
| Sidebar — Attachments panel | **RETAIN (compat / advanced)** | `Pdf4QtLibGui/pdfsidebarwidget.ui` | inherited | Attachment management — Compatibility/advanced feature, not primary | advanced / compatibility | 1 | N/A no action | defer to 1.1B | `attachmentsButton` (`pdfsidebarwidget.ui:172`), `attachmentsPage` (`:494`), `attachmentsTreeView` (`:509`); not a menu suite | **RESOLVED** — sidebar panel, not standalone plugin. Security hardening tracked separately (#63 verified) |
| PDFWidgetFormManager (form fill/read) | **RETAIN (compat, fill-only)** | `Pdf4QtLibWidgets/sources/pdfwidgetformmanager.*` | inherited | Form creation/editing — **Likely remove from primary UI**; Form filling — Possibly retain as compatibility | compatibility (fill-only, no authoring UI) | 1 | N/A no action | defer to 1.1B | Instantiated `Pdf4QtLibGui/pdfprogramcontroller.cpp:456`; no standalone Forms plugin or menu | **RESOLVED** — infrastructure only, not authoring UI. Hide field chrome in Loupe shell? |

---

## Packaging / distribution surfaces

The desktop row below records the original 1.1A audit snapshot. For the current
release/developer split, use `docs/product-surface.json`: the `loupe-release`
profile has one Editor desktop entry and one Editor AppX application, while the
developer profile retains compatibility entries without presenting them as the
Loupe product shell.

| Surface | Recommendation (Prune / Retain) | Current location (path) | PDF4QT origin | Notion disposition | Target Loupe surface | Pruning level recommended | Sync durability | Timing | Evidence | Open questions |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `PDF4QT_LOUPE_DISTRIBUTION` CMake option | **RETAIN (flip default ON)** | root `CMakeLists.txt` (~68-100) | modified (Loupe) | Product pruning levels 2/3 policy | Default **ON** for Loupe release builds | N-A (policy) | N/A already flag-gated | defer to 1.1B (flip default) | `option(...)` at `CMakeLists.txt:68` (currently `OFF`); disables Viewer, PageMaster, Diff, LaunchPad, CodeGenerator, JBIG2_Viewer, ExampleGenerator, AudioBook, Scanner when ON | Flip default without breaking dev/CI full-matrix builds? |
| `Desktop/*.desktop` set | **PRUNE (consolidate)** | `Desktop/` | modified (Loupe IDs) | Stop shipping five products (1.1B) | Loupe Editor + aligned Linux entries only | 2 | flag-gated (remove or guard `.desktop` install rules when `PDF4QT_LOUPE_DISTRIBUTION`) | defer to 1.1B | **5** `.desktop` files: main `io.github.mberrys.Loupe-pdf.desktop` → `Exec=Pdf4QtLaunchPad %f`; Editor (`Exec=Pdf4QtEditor %f`), Viewer (`Pdf4QtViewer %f`), Diff (`Pdf4QtDiff %F`), PageMaster (`Pdf4QtPageMaster`) each own `.desktop`; **no** PdfTool/LaunchPad-specific shipping beyond main | Consolidate to single Loupe `.desktop`? |
| Flatpak manifest (app) | **RETAIN** | `Flatpak/io.github.mberrys.Loupe-pdf.json` | modified (Loupe) | Loupe + Loupe CLI product model | Single app-id, Editor entrypoint | 1 | N/A no action | defer later (#40 permissions) | `"command": "Pdf4QtEditor"` (`:8`); `-DPDF4QT_PLUGIN_OCR=OFF` (`:131`); `--filesystem=host` (`:15`) plus speech-dispatcher run/cache mounts | Bundle PdfTool in Flatpak as `loupe-cli`? |
| Flatpak Flathub manifest | **RETAIN** | `Flatpak/flathub.json` | modified (Loupe) | Loupe + Loupe CLI product model | Flathub build config for single app-id | N-A | N/A no action | defer later | Present alongside the app manifest; governs Flathub publishing config (not the app command itself) | Keep in sync with app manifest when consolidating `.desktop`? |
| WiX installer (`Product.wxs.in` + `WixInstaller/CMakeLists.txt`) | **RETAIN (conditional features)** | `WixInstaller/` | inherited + Loupe fragments | Stop presenting upstream executable names | Editor + PdfTool + plugin pack (+ optional OCR service) | 2 (conditional features) | flag-gated (CMake already omits Viewer/PageMaster/Diff when build flags OFF) | defer to 1.1B | **Finding:** static `Product.wxs.in` has **zero** hardcoded `Pdf4QtDiff`/`Viewer`/`PageMaster`/`LaunchPad` names; always ships `Pdf4QtEditor` + `PdfTool` + core plugins (Dimensions, ObjectInspector, OutputPreview, Redact, SoftProofing, Signature, Editor, LoupePreflight — `Product.wxs.in:285-306`). Optional: AudioBook (`:284`), OCR (`:309`), Scanner (`:310`) via CMake substitutions. **Linux inconsistency:** Desktop exposes 4 apps + LaunchPad hub while default WiX slim path is Editor-centric | **RESOLVED** — WiX vs Linux packaging mismatch documented; not fixed in 1.1A. Align Desktop set when `PDF4QT_LOUPE_DISTRIBUTION` defaults ON |

---

## Recommended pruning-level-1 candidates for 1.1B

*Follow-up backlog only — not implemented in 0.1.1A.*

1. **Hide Developer menu** (`actionShow_Text_Blocks`, `actionShow_Text_Lines`) —
dev/QA flag or advanced prefs.
2. **Default `PDF4QT_LOUPE_DISTRIBUTION=ON`** for release/CI packaging jobs —
stops shipping Viewer, PageMaster, Diff, LaunchPad, AudioBook, Scanner, and
dev tools while keeping sources for upstream sync.
3. **De-emphasize Insert annotation-authoring suite** — move to contextual/
advanced or remove from primary menus per Notion.
4. **Hide Certificate Manager** unless signature workflows require it in V1.
5. **Collapse Linux `.desktop` proliferation** to match Windows/Flatpak
single-app model.

---

## OPEN items (grep `OPEN` in this file)

| Token | Topic | Blocker |
| --- | --- | --- |
| **OPEN** | Redaction product commitment — Editor plugin + PdfTool `redact` + verifier tests | #66 |
| **OPEN** | Compare / Diff — production-proof bar before shipping in-app Compare | Product decision |
| **OPEN** | `AGENTS.md` split-surface vs. Notion 1.1B convergence | 1.1B architecture ADR / `AGENTS.md` update |

---

## Menu action coverage index

Every non-separator `action*` in `pdfeditormainwindow.ui` maps to exactly one row
above:

| Menu / area | Manifest row |
| --- | --- |
| `menuFile` | File menu |
| `menuEdit` (find/text) | Edit — text selection / find |
| `menuEdit` (encryption…bitonal) | Edit — production fixup cluster |
| `menuView` | View menu |
| `menuInsert` | Insert — annotation-authoring suite |
| `menuGoTo` | Go To menu |
| `menuTools` (magnifier…reset) | Tools — inspection utilities |
| `menuTools` (certificates) | Tools — Certificate Manager |
| `menuHelp` | Help menu |
| `menuDeveloper` | Developer menu |
| Sidebar attachments | Sidebar — Attachments panel |
| Form widgets on canvas | PDFWidgetFormManager |

Plugin-provided menus/toolbars are out of scope for this `.ui` grep; they are
covered in **Editor plugins**.
