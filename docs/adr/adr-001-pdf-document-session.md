# ADR-001: PDFDocumentSession lifetime, caching, and thread-safety

**Status:** implemented
**Implemented-at:** d1cb0fa0ab250fc59a77128851068de3a7aefb97
**Last-verified:** 2026-09-13 @ e65cdd19e0fb876adf367f0387c63996efcfebfa
**Superseded-by:** none
**Date:** 2026-07-12
**Deciders:** MIC-151 epic review

## Context

MIC-151 introduces `PDFDocumentSession` as a shared Core session that owns a
`PDFDocument` plus compile-cache and decoded-stream cache. It must be reusable
from PdfTool (headless), Editor (GUI), and PageMaster (batch). Issue #236 made
`PDFDocumentContext` the revision authority; session caches must not return
results for a superseded `PDFRevisionIdentity`.

## Decision

- **Ownership:** `PDFDocumentSession` takes `PDFDocument*` (non-owning reference)
  and an optional `PDFDocumentContext*`. The session does not delete the document;
  the caller owns the document lifetime. Factory helpers `create()`,
  `createForInspection()`, and `destroy()` keep DLL allocation boundaries explicit.
- **Revision authority:** When a context is wired, `getRevision()` and
  `isCurrent()` delegate to `PDFDocumentContext`. Without a context, the session
  carries a local document identity and revision counter. Cache keys include the
  active `PDFRevisionIdentity` so stale compiled pages and decoded streams are
  never reused after a mutation or profile/renderer change. See
  [`REVISION_CONTEXT.md`](../REVISION_CONTEXT.md).
- **Admission:** `PDFDocumentSessionAdmission::Managed` reserves the full resource
  envelope for interactive work. `Inspection` is the lightweight PdfTool preflight
  path (`createForInspection()`).
- **Compile cache:** `std::map<PageCacheKey, PDFPrecompiledPage>` plus an
  insertion-order queue. `PageCacheKey` pairs revision with zero-based page index.
  Populated lazily via `compilePage()`, bounded to eight entries by default and
  additionally by compiled-page byte limits (`CompiledCacheByteLimitDefault`,
  configurable via `setCompiledCacheByteLimit()` / `setCacheLimit()`).
- **Decoded-stream cache:** `std::map<StreamCacheKey, QByteArray>` plus an
  insertion-order queue. `StreamCacheKey` pairs revision with
  `PDFObjectReference`. Populated lazily via `getDecodedStream()` and bounded to
  256 entries by default, with an independent byte cap.
- **Invalidation:** Call `invalidate()` to clear all caches. Document mutations,
  renderer-feature changes, processing-limit changes, and revision advances all
  require fresh cache entries. `shedPrefetchAndQuality()` shrinks cache caps under
  memory pressure before dropping interaction work.
- **Rendering helpers:** The session owns the renderer, font cache, CMS, optional
  content activity, processing budget, and optional page-cache budget used by
  preflight and rendering tools.
- **Thread-safety:** The session is not thread-safe. A single synchronous run
  owns its session; concurrent page evaluation requires separate sessions or
  external synchronization.
- **Namespace:** `pdf::PDFDocumentSession` in `LoopLibCore`.

## Consequences

- Preflight checks that render the same page twice (e.g., Tier-1 bleed + Tier-2
  content-bleed for the same page) benefit from the compile cache while the
  revision is current.
- Editor, PageMaster, and PdfTool construct a session around their document
  while retaining document ownership in the caller.
- Bounded caches keep hostile or unusually large documents from retaining every
  compiled page and decoded stream for the life of a run.
- Async or cross-thread producers must compare `PDFRevisionIdentity` at the
  presentation boundary; mismatches are discarded, never reconciled heuristically.
