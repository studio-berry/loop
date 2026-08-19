# ADR-001: PDFDocumentSession lifetime, caching, and thread-safety

**Status:** implemented
**Implemented-at:** d1cb0fa0ab250fc59a77128851068de3a7aefb97
**Last-verified:** 2026-08-10 @ 589133449398f029d8b6624b01b49aa4b3343591
**Superseded-by:** none
**Date:** 2026-07-12
**Deciders:** MIC-151 epic review

## Context

MIC-151 introduces `PDFDocumentSession` as a shared Core session that owns a
`PDFDocument` plus compile-cache and decoded-stream cache. It must be reusable
from PdfTool (headless), Editor (GUI), and PageMaster (batch).

## Decision

- **Ownership:** `PDFDocumentSession` takes `PDFDocument*` (non-owning reference).
  The session does not delete the document; the caller owns the document lifetime.
- **Compile cache:** `std::map<size_t, PDFPrecompiledPage>` plus an insertion-order
  queue, keyed by zero-based page index. Populated lazily via `compilePage()` and
  bounded to eight entries.
- **Decoded-stream cache:** `std::map<PDFObjectReference, QByteArray>` plus an
  insertion-order queue. Populated lazily via `getDecodedStream()` and bounded
  to 256 entries.
- **Invalidation:** Call `invalidate()` to clear all caches. Typically called
  when the underlying document is mutated. Changing renderer features or
  processing limits also invalidates the relevant cached state.
- **Rendering helpers:** The session owns the renderer, font cache, CMS, optional
  content activity, and processing budget used by preflight and rendering tools.
- **Thread-safety:** The session is not thread-safe. A single synchronous run
  owns its session; concurrent page evaluation requires separate sessions or
  external synchronization.
- **Namespace:** `pdf::PDFDocumentSession` in `LoupeLibCore`.

## Consequences

- Preflight checks that render the same page twice (e.g., Tier-1 bleed + Tier-2
  content-bleed for the same page) benefit from the compile cache.
- Editor, PageMaster, and PdfTool construct a session around their document
  while retaining document ownership in the caller.
- Bounded caches keep hostile or unusually large documents from retaining every
  compiled page and decoded stream for the life of a run.
