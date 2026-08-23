# Resource-envelope and lifecycle qualification

Phase 3 uses `docs/RESOURCE_ENVELOPE_BUDGETS.json` as the immutable pre-run
budget contract. The checked-in safety caps are blocking. Cross-platform
baseline deltas are reported separately until both Linux and Windows baselines
exist.

Every result must include the corpus, manifest, and workload digests together
with `PDFRunIdentity` and the exact candidate commit. `-1` means a measurement
was unavailable; the result must then carry `status: incomplete` or another
non-success status and an `incomplete_reason`.

PdfTool benchmark output and integrated document-session output are separate
evidence records. A Quick first-view record remains `incomplete` until the
Quick product path is implemented in Phase 4.

## Qualification sequence

1. Validate the external DIV2K corpus and generate one canonical manifest with
   `--hash-all`.
2. Build the deterministic 10,000-page image-heavy PDF and record its digest.
3. Run PdfTool benchmark profiles on Linux and Windows with the same manifest.
4. Run the integrated session/scheduler harness with the same workload identity.
5. Replay the bounded lifecycle trace corpus on both platforms.
6. Attach JSON results, digests, platform identities, and dispositions to the
   candidate-SHA evidence dossier.

No unavailable measurement may be converted to zero or treated as a pass.
