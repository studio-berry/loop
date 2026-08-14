# Atomic, collision-safe, overwrite-explicit outputs — Implementation Plan

Status: **in review** (planning doc, M0 — locked before code).
Scope: Loupe-pdf 0.1.0-alpha, branch `dev`.
Issue: [studio-berry/loupe#10] — "Make generated PDF and attachment outputs atomic, collision-safe, and overwrite-explicit".
Phase 1 (MIC-309, PR #46/#47) landed: `PdfTool unite`/`separate` write through `PDFDocumentWriter(safeWrite=true)` and PageMaster got atomic per-output writes plus a batch manifest. This document covers Phase 2 — every remaining generated-file output in the tree.

## Goal

One Core write helper used by every output surface, one consistent overwrite flag across PdfTool, and an
invariant that no code path can destroy an existing output before the replacement bytes are durable.

Three defect classes are closed:

- **Not atomic** — a crash, cancel, or full disk must never leave a truncated file where a valid one used to be.
- **Not overwrite-explicit** — no generated output is clobbered silently; `--overwrite` is the canonical
  flag, `--force` is kept only as a silent alias so existing scripts keep working.
- **Not collision-safe** — names that collide *within a single run* get a unique suffix; losing an
  attachment is data loss regardless of what the user asked for.

Two worse-than-not-atomic defects are folded in: `loupepreflightplugin.cpp:808-811` deletes an existing
output up front (defeating the `QSaveFile` guarantee), and `signatureplugin.cpp:494-499` swallows the
signed-document write result.

## Non-goals

- Changing `PDFDocumentWriter::write`s signature — the fix at PDF call sites is `safeWrite=true` consistently.
- Making PdfTool interactive — destructive fixups stay non-interactive (`--overwrite` / `--dry-run` / `--report`).
- Collision-renaming files that already exist on disk across runs (that is `--overwrite` semantics, not `makeUniqueFileName`).
- `OCRPlugin` / `LoupePreflightPlugin` snapshot writes and `PdfTool ocr` page PNGs: they target `QTemporaryDir`, where the non-atomic path is correct.
- **Known gap (documented):** `PdfTool audiobook` streams through Windows SAPI (`ISpeechFileStream`), which cannot be redirected through `QSaveFile` without restructuring the feature. Left as-is; noted here so future work can revisit it.

## Spec locks

| Lock | Decision |
|------|----------|
| Helper name/API | `PDFSafeFileWriter` (`pdfsafefilewriter.*`), static `writeData` / `writeDevice` / `makeUniqueFileName` in `namespace pdf` |
| Result type | `PDFOperationResult` (already `PDFDocumentWriter::write`'s result type) |
| Atomic mechanism | `QSaveFile` + `setDirectWriteFallback(true)`; short write / failed commit / failed shutdown → error; `cancelWriting()` on producer failure |
| Overwrite flag (PdfTool) | `--overwrite` canonical; `--force` registered as a **silent alias** only on commands that already accept it |
| `add-bleed --force` | Kept as-is — means "ignore the skip-if-already-bleeding heuristic", never overwrite; `add-bleed` gets `--overwrite`/`--dry-run`/`--report` via `DestructiveWrite` |
| Field name | `PDFToolOptions::destructiveForce` → `destructiveOverwrite`, single consumer `validateDestructiveOutput` |
| Intra-run collisions | `makeUniqueFileName()` — names colliding within a run always get `"base (n).ext"`; a pre-existing on-disk file is governed by `--overwrite` |
| PageMaster manifest | Manifest / resume / rollback logic unchanged; only `writeDocumentAtomically` is swapped out |
| Deletions | `removePartialOutput()` is deleted once its last two callers (`unite`, `redact`) stop calling it |

## API

```cpp
namespace pdf {
class PDF4QTLIBCORESHARED_EXPORT PDFSafeFileWriter
{
public:
    enum class OverwritePolicy { Fail, Overwrite };

    static PDFOperationResult writeData(const QString& fileName, const QByteArray& data, OverwritePolicy policy);
    static PDFOperationResult writeDevice(const QString& fileName,
                                          const std::function<bool(QIODevice*)>& producer,
                                          OverwritePolicy policy);
    static QString makeUniqueFileName(const QString& fileName);
};
}
```

### Overwrite policy semantics

- `Fail` — if the target already exists, return an error naming the path and write nothing.
- `Overwrite` — proceed; `QSaveFile` still replaces the previous bytes only after the new bytes are durable.

### Intra-run uniquification rule

`makeUniqueFileName()` returns `fileName` when free, otherwise the lowest `"base (n).ext"`. Used only for
names that collide *within a run* (attachment saves). Files already on disk are the `--overwrite` domain:
without the flag, fail before writing anything and name the offending path.

## Surface order

1. **Core** — `PDFSafeFileWriter` (+ `Pdf4QtLibCore/CMakeLists.txt`); `pdfpagemasterexport.cpp` swaps its file-local `writeDocumentAtomically` and manifest/preflight `writeFileAtomically` onto `PDFSafeFileWriter`. Regression signal: `tst_pagemasterexporttest.cpp` stays green unmodified.
2. **PdfTool** — one flag: `registerDestructiveWriteOptions(bool registerForceAlias)`; `--overwrite` canonical; `--force` alias except `add-bleed`. Command table below.
3. **Gui / Editor plugins / Diff** — direct `PDFSafeFileWriter` use (all depend on Core).
4. **Tests** — `tst_safefilewritertest.cpp` (new Core tests) + `tst_operatoracceptance.cpp` (real-CLI acceptance).

### PdfTool commands changed

| Command | Change |
|---------|--------|
| `add-bleed` | Drop bespoke `QFile::exists`+`addBleedOverwrite`; `DestructiveWrite` flag; reports driven by `destructiveReport`/`destructiveDryRun` |
| `remove-external-links` | In-place rewrite of `options.document`; add `DestructiveWrite` + `validateDestructiveOutput` (matches `optimize`/`encrypt`/`decrypt`) |
| `attachments --att-save-all` | `DestructiveWrite`; guard all outputs up front; atomic `writeData`; in-run collisions get `makeUniqueFileName`; effective names reported |
| `render` / `fetch-images` | `DestructiveWrite`; guard; atomic `QImageWriter` over `writeDevice` |
| `unite`    / `redact` | Drop `removePartialOutput()` pre-write cancel calls |

### Non-PdfTool surfaces

| File | Change |
|------|--------|
| `Pdf4QtLibGui/pdfsidebarwidget.cpp` | Attachment save → `writeData` |
| `Pdf4QtLibGui/pdfbookmarkmanager.cpp` | Bookmark JSON export → `writeData`; surface open/write failures through the caller |
| `Pdf4QtLibGui/pdfrendertoimagesdialog.cpp` | Rendered-image write → atomic `writeDevice` |
| `SignaturePlugin/signatureplugin.cpp` | Signed doc → `writeData`, **and report failures** (currently silent) |
| `RedactPlugin/redactplugin.cpp` | `writer.write(..., false)` → `true` |
| `LoupePreflightPlugin/loupepreflightplugin.cpp` | Delete `QFile::remove(outputPath)` before the atomic write |
| `Pdf4QtDiff/mainwindow.cpp` | XML export → `writeDevice` wrapping `saveToXML`; report write → unconditional `safeWrite=true` |

Confirmation prompts stay in Editor/PageMaster (never in PdfTool). `loupepreflightplugin.cpp:783-791` overwrite prompt is the pattern.

## Related code

| Area | Path |
|------|------|
| Safe writer | `Pdf4QtLibCore/sources/pdfsafefilewriter.{h,cpp}` (new), `Pdf4QtLibCore/CMakeLists.txt` |
| Existing QSaveFile pattern | `Pdf4QtLibCore/sources/pdfdocumentwriter.cpp:192-217` |
| PageMaster swap | `Pdf4QtLibCore/sources/pdfpagemasterexport.cpp:326-343` |
| Flag plumbing | `PdfTool/pdftoolAbstractApplication.{h,cpp}` |
| Write-path hooks | `PdfTool/pdftoolattachments.cpp`, `pdftoolremoveexternallinks.cpp`, `pdftooladdbleed.cpp`, `pdftoolrender.cpp`, `pdftoolfetchimages.cpp`, `pdftoolunite.cpp`, `pdftoolredact.cpp` |
| Planning process | `docs/PLANNING.md` |
| Bug-hunt root | `docs/BUG_HUNT_2026-08-04.md` (§1: `removePartialOutput` reasoning) |

## Implementation status

Implementation completed on `dev` (uncommitted):

- **Core**: `PDFSafeFileWriter` added (`pdfsafefilewriter.{h,cpp}`, `Pdf4QtLibCore/CMakeLists.txt`); `pdfpagemasterexport.cpp` swaps its file-local `writeDocumentAtomically` onto `PDFDocumentWriter::write(..., safeWrite=true)` and routes the manifest/preflight `writeFileAtomically` through `PDFSafeFileWriter::writeData`. `tst_pagemasterexporttest.cpp` untouched.
- **PdfTool**: one shared `DestructiveWrite` flag — `--overwrite` canonical, `--force` a registered legacy alias except on `add-bleed` (`add-bleed --force` keeps its heuristic meaning). `PDFToolOptions::destructiveForce` → `destructiveOverwrite`; added `validateDestructiveOutputs()`; deleted `removePartialOutput()` and its unite/redact callers. `add-bleed`, `remove-external-links`, `attachments`, `render`, `fetch-images` now guard outputs up front (no silent clobber) and write via the safe path; `attachments` uniquifies intra-run collision names.
- **Surfaces**: bookmark JSON export + sidebar attachment save → `writeData` (failures surfaced); render-to-images dialog → atomic `writeDevice`; Diff XML report → `writeDevice`, report PDF → unconditional `safeWrite=true`; RedactPlugin → `safeWrite=true`; SignaturePlugin signed-doc → `writeData` and failures now reported; LoupePreflightPlugin no longer deletes the existing output before the atomic write.
- **Tests**: `tst_safefilewritertest.cpp` (new Core test, `UnitTestsSafeFileWriter` target) + overwrite-explicit CLI acceptance slot in `tst_operatoracceptance.cpp`.

Builds were not run (repo rule); the Cursor Cloud build/tests steps are in the AGENTS.md dev build notes.
