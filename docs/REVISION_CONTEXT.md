# Document revision context

`PDFDocumentContext` is the revision authority for document-bound work. It owns
the active artifact identity, its monotonically increasing `DocumentRevision`,
the cache generation, the effective profile identity, and the shared
`PDFDocumentSession`.

Every cache entry or asynchronous result that can outlive a mutation carries a
`PDFRevisionIdentity`. The 0.1.1 spec name `PDFRevisionToken` is an alias of
that type (`using PDFRevisionToken = PDFRevisionIdentity`); it is not a second
freshness struct. Persisted provenance and the artifact store keep using
`PDFArtifactIdentity`. Consumers must compare the complete value with
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

`UnitTestsRevisionStress` runs the concurrent form of the same contract.
Render, preflight, thumbnail, and repair-plan jobs are in flight together
while the document is mutated and the effective profile changes at points the
producers do not observe, and the test asserts the four correctness
properties: zero stale findings applied, zero stale tiles presented past an
invalidation boundary, deterministic cancellation (cancelled work is terminal,
is never success, and publishes nothing), and no cache read returning a result
for the wrong revision. Zero stale results is a correctness requirement, not a
percentile, so a single admitted stale result fails the test.

Timing alone cannot prove that the fence was exercised, so the test does not
rely on it: one phase submits results against the current revision and asserts
they are admitted and read back as current, and a second phase holds one
producer per job kind inside its work function, mutates the document underneath
all of them, and only then releases them - asserting both the consumer-side
rejection and the scheduler-side `Stale` outcome.
