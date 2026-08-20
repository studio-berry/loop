# ADR-006: Rotating logs, log scrubbing, and the diagnostics bundle

**Status:** implemented
**Implemented-at:** 24e9ba8bc88d85c7ddc59a7320faba94d382d76b
**Last-verified:** 2026-08-10 @ 589133449398f029d8b6624b01b49aa4b3343591
**Superseded-by:** none
**Date:** 2026-08-09
**Deciders:** gh-15 (mirrors upstream mberrys/Frisket-pdf, issue 81)

## Context

Loupe had no application logging: zero use of `qInstallMessageHandler` or
`QLoggingCategory` repo-wide, `qWarning` used twice in shipping GUI code
(`LoupeLibGui/pdfwintaskbarprogress.cpp`), and the support story documented
in `docs/PRODUCTION_RUNBOOK.md` was "ask the user to run the failing command
in a terminal and attach stderr." Diagnostic signal that does exist today is
thrown away: `PDFRenderError`s die with the widget, preflight sidecar stderr
is bounded then discarded, OCR sidecar stderr is dumped into a modal and
cleared. The only persisted failure artifact is Sentry/crashpad — Windows-only
in practice, off by default elsewhere, and explicitly unable to be scrubbed:
minidumps can carry PDF content and file paths (R-008 in
`docs/V1_RELEASE_READINESS.md`). A support engineer had nothing to ask for
except a terminal re-run, and the user's only channel for real crash data was
the one that leaks document content.

## Decision

- **Log location** mirrors `PDFSentrySession`'s `databasePath()` in structure,
  but adds a portable-install branch Sentry does not have:
  1. `LOUPE_LOG_DIR` environment variable, if set;
  2. `<settingsPath>/logs`, when `PDFSettings::getSettingsPath()` is non-empty
     (portable / `--config` installs — Editor only, PdfTool has no `--config`);
  3. `QStandardPaths::AppLocalDataLocation` + `/logs`, falling back to the temp
     directory.
  Logs land beside the existing `sentry-native` crash DB, and `--config`
  portable installs stay self-contained. Because portable mode depends on the
  command-line parser having already run, `PDFLogSession` is constructed
  *after* `PDFSettings::applyCommandLineSettingsPath()` in the Editor's
  `main()` — one step later than `PDFSentrySession`, which is constructed
  before argument parsing and therefore does **not** honor `--config` (a
  pre-existing property of Sentry's crash DB path, not something this change
  fixes).
- **Rotation:** append to `<applicationId>.log`; at 2 MiB roll to `.log.1`,
  keep 3 files total (`.log`, `.log.1`, `.log.2`). Bounded footprint by
  construction — rotation always shifts exactly two backups and prunes
  anything beyond `.log.2`, so there is never a `.log.3` to accumulate.
- **Scrubbing is a property of the sink, not a call-site convention.** Every
  message is passed through `pdf::PDFLogScrubber::scrub()` inside the
  installed `qInstallMessageHandler` callback, before the line is written.
  This is the direct answer to the R-008/A10 correction in
  `docs/V1_RELEASE_READINESS.md`: that audit found the Sentry "no PDF content"
  claim was documentation with nothing in code enforcing it. Log scrubbing
  does not repeat that mistake — the guarantee is enforced in the one place
  every message passes through, not documented as a hope at each of the
  (many, growing) call sites.
- **Basename dropped, extension kept** for any absolute path scrubbing does
  not already resolve to `<HOME>`/`<TEMP>`: `<PATH:.pdf>`, never
  `<PATH:Acme_Q3_Contract.pdf>`. This is the single most load-bearing design
  decision in the change. In this product the filename is usually the PII —
  customers name PDFs after the people, companies, and matters they concern —
  while the extension is the only part of the path with diagnostic value.
- **The message handler chains to whatever was previously installed**,
  restoring it on destruction, so PdfTool's existing stderr behavior is
  unchanged; the log file is additive, not a replacement output.
- **Level control** reads `LOUPE_LOG_LEVEL`, else the `diagnostics/logLevel`
  `QSettings` key, else `Warning`. Core reads that key itself (via the
  standard `QSettings(QSettings::IniFormat, QSettings::UserScope,
  organizationName(), applicationName())` idiom already used ~20 times in the
  repo) rather than going through `PDFViewerSettings`, which lives in Gui —
  so PdfTool gets the same setting without depending on the Gui module.
- **Diagnostics bundle is a plain directory + `manifest.json`.** No new
  dependency, no Qt private modules. `manifest.json` follows the versioned-
  schema precedent already established by `LOUPE_PREFLIGHT_SCHEMA_VERSION`.
  Every file is written with `QSaveFile` and the whole bundle directory is
  removed on any failure, so a partial bundle is never left behind — the same
  discipline as the PageMaster manifest-rollback work (A9/A23 in
  `docs/V1_RELEASE_READINESS.md`).
- **Never in a bundle:** any PDF, document content, the recent-files list, or
  a crash minidump. The minidump exclusion is explicit and documented in the
  bundle's own `README.txt`, not merely implied by omission — crash
  minidumps remain a separate, opt-in mechanism (`SENTRY_DSN`) with a weaker,
  disclosed privacy property (R-008) that this feature does not change or
  paper over.
- **Surfaces:** Editor and PdfTool, with the API living in Core
  (`LoupeLibCore/sources/pdflogger.*`, `pdflogscrubber.*`,
  `pdfdiagnostics.*`) so Viewer/PageMaster/Diff can adopt the same logger and
  collector later without duplicating the scrubbing logic.

## Consequences

- A support engineer can ask for one artifact (`PdfTool diagnostics --output
  <dir>`, or Editor → Help → Collect Diagnostics…) instead of a terminal
  transcript, and the artifact is safe for the user to inspect before sending.
- Any future `qWarning`/`qDebug`/`qCWarning` call site is automatically
  captured and scrubbed once a session is active — no call site has to
  remember to scrub, and none can accidentally bypass it.
- The portable-install (`--config`) log path only applies to the Editor;
  PdfTool always resolves to `AppLocalDataLocation` (or `LOUPE_LOG_DIR`) since
  it has no `--config` option today. Adding one is out of scope for this
  change.
- `PDFSentrySession`'s crash DB still does not honor `--config` — this ADR
  does not change Sentry's behavior, only documents that the new logger is
  intentionally more portable-aware than it.
