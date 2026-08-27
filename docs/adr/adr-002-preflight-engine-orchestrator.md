# ADR-002: PreflightEngine orchestrator boundary

**Status:** implemented
**Implemented-at:** d7f517f8dd397e50131b896fba34d243950a9779
**Last-verified:** 2026-08-10 @ 589133449398f029d8b6624b01b49aa4b3343591
**Superseded-by:** none
**Date:** 2026-07-12
**Deciders:** MIC-151 epic review

## Context

MIC-154 refactors the flat loop in `PdfTool/pdftoolpreflight.cpp` into a reusable
`pdf::PreflightEngine` in Core. The engine is callable from PdfTool (headless
CLI), the Editor sidecar integration, and PageMaster's batch preflight gate.

## Decision

- **Location:** `LoopLibCore/sources/preflightengine.h/.cpp`, namespace `pdf`.
- **Construction:** `PreflightEngine(PDFDocumentSession*)`. The engine borrows
  the session; the caller manages session lifetime.
- **API:** `PreflightResult run(const QJsonObject& profile)` or
  `PreflightResult run(const PreflightProfileData& profile)`. `PreflightResult::toJson()`
  emits normalized report schema version 3, including inspection completeness,
  per-check statuses, findings, evidence, decisions, and optional PDF/X results.
- **Check registry:** Checks are registered by string ID → callable. The built-in
  catalog is generated from `registerBuiltInChecks()` and includes bleed, trim,
  page-size, processing-steps/dieline, content-bleed, ink-coverage, color-mode,
  transparency-risk, thin-strokes, color-inventory, output-intent,
  embedded-fonts, font-integrity, hidden-content variants, image-resolution,
  and white-overprint. Callers can replace or add checks with `registerCheck()`.
- **Execution:** Profile checks run in declared order. Disabled checks become
  `skipped`; unknown checks become `unsupported`; contained failures and budget
  exhaustion remain visible in the normalized result. PDF/X policy reduction is
  fail-closed, and applicable fixups are filtered after findings are collected.
- **Raster constraints:** The engine (and `PDFBleedMarginProbe`) use `PDFRasterizer`
  via `RendererEngine::QPainter`. On GUI platforms, rasterizer construction must
  happen in the main thread. PdfTool satisfies this; Editor plugin shells out via
  QProcess and is unaffected. In-process Editor/PageMaster integration (P2) must
  respect the main-thread constraint or use `PDFRasterizerPool`.
- **Thread-safety:** The engine is not thread-safe. A single run is synchronous.

## Consequences

- `PdfTool/pdftoolpreflight.cpp` becomes a thin driver: load profile →
  create session → create engine → run → write JSON.
- The existing checks and their shared math live in Core beside the engine;
  PdfTool remains a thin profile/load/write driver.
- The plugin's QProcess path consumes the normalized report and does not own a
  second check registry. Any future in-process integration must respect the
  session and renderer's synchronous, non-thread-safe boundary.
