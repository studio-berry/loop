# Document revision context

`PDFDocumentContext` is the revision authority for document-bound work. It owns
the active artifact identity, its monotonically increasing `DocumentRevision`,
the cache generation, the effective profile identity, and the shared
`PDFDocumentSession`.

Every cache entry or asynchronous result that can outlive a mutation carries a
`PDFRevisionIdentity`. Consumers must compare the complete value with
`PDFDocumentContext::getRevision()` before presenting or storing a result. A
mismatch is discarded; it is never reconciled heuristically.

Document replacement and `PDFModifiedDocument` mutations advance both the
document revision and cache generation. Renderer, processing-limit, or profile
changes advance the cache generation without claiming that the PDF bytes
changed. `PDFDocumentSession` cache keys include the revision, while the editor
page compiler, text-layout compiler, thumbnail renderer, and PageMaster preview
renderer reject superseded results.

The `UnitTestsDocumentSession/revisionFence_rejectsSupersededResults` test
repeats the mutation/result-boundary check across 512 revision transitions.
This is the deterministic unit-level form of the hostile-workload stress
contract: render, preflight, thumbnail, and repair-plan producers may finish
in any order, but only a result carrying the current revision may cross the
presentation/cache boundary.
