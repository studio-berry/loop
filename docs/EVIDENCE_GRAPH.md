# Evidence Graph

Status: Wave B (S06–S08). Core types live in `pdfevidencegraph.h`.

Preflight families **images**, **colorants**, **strokes**, **overprint/transparency**, and **fonts** share one collector walk. `PDFEvidenceCollector::collect()` writes a revision-bound `PDFEvidenceGraph`. Graph-backed checks then evaluate those records; they do not walk page content themselves.

`thin-parts`, bleed/geometry, output-intent, and font-integrity stay on their existing processors.

## Fail-closed

An incomplete graph (`complete == false` or a non-empty `incompleteReason`) cannot reduce to PASS. `PreflightEngine::run()` records `errorCode` `evidence-incomplete`, marks graph-backed checks incomplete, and `reducePreflightVerdict()` returns `Incomplete`.

## Finding citations

Graph-backed findings include `evidence_ids` naming the records that produced them. Golden-corpus snapshots strip that field so existing reports stay stable.

## JSON envelope

`PDFEvidenceGraph::toJson()` writes schema kind `evidence-graph` version `1.0` via `writeSchemaEnvelope()`.
