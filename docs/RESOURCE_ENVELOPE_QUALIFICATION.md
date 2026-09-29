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

## Hosted qualification (issue #19)

`.github/workflows/resource-envelope-qualification.yml` is the qualifying run.
It triggers on `workflow_dispatch` and on pull requests that touch the
benchmark, the envelope scripts, or the budget contract. Each Linux and Windows
job:

1. Builds `PdfTool` from the candidate SHA.
2. Generates the fixture bundle with
   `scripts/resource_envelope/synthetic_workload.py`. The office, 500 MB
   image-heavy, and 10,000-page fixtures are deterministic synthetic PDFs
   (SHAKE-256 noise images stored with FlateDecode), so hosted runners need
   no external corpus. The 10,000-page fixture uses the
   `synthetic-image-heavy` workload caps, which equal the DIV2K caps.
3. Runs `run_matrix.py --strict --repetitions 3` with:
   - a measured preflight phase (`benchmark --profile`, default
     `loop-preflight/profiles/loop-default.json`), so a clean run reports
     `status: complete` with a real `preflight_high_water_bytes`. Fixtures
     above 1,000 pages (the 10,000-page one) preflight their first 256 pages
     only (`--preflight-page-last 256`, recorded as
     `profile.preflight_page_last`); rendering still covers every page;
   - a cancellation probe on `ten-thousand-page`, which interrupts a render-only
     run (no preflight phase, whose document-wide setup does not poll for
     cancellation)
     and requires a `cancelled` envelope within the workload's
     `cancellation_latency_ms`;
   - a recovery probe, which times a fresh process reopening the same
     fixture and rendering its first page (`recovery_ms`, within the
     workload's `recovery_ms`);
   - a hostile lane over `UnitTests/testdata/budget_exhaustion/`, where each
     PDF must end in a contained PdfTool exit code (rejection is fine) within
     the hostile timeout, without breaching the resident ceiling.

The `evidence` job combines both matrices with
`scripts/qualification/build_resource_envelope_evidence.py` into a schema
version 2 record that carries the run id and URL, and validates it with
`validate_resource_envelope_evidence.py`. The disposition is `passed` only when
both platforms passed strictly on the same candidate SHA.

A crash (an exit code outside PdfTool's defined codes, or `InternalError`), a
timeout, a non-success exit, or a missing envelope never produces a
`measured` fixture. Crashes and timeouts fail the record outright.

## External DIV2K sequence

The original DIV2K qualification remains available for local runs:

1. Validate the external DIV2K corpus and generate one canonical manifest with
   `--hash-all`.
2. Build the deterministic 10,000-page image-heavy PDF and record its digest.
3. Create an external fixture manifest using the schema at
   `docs/schemas/resource-envelope-fixtures.schema.json`, then run
   `scripts/resource_envelope/run_matrix.py --manifest ... --strict` with the
   same probe and hostile options as the hosted workflow.
4. Run the integrated session/scheduler harness with the same workload identity.
5. Replay the bounded lifecycle trace corpus on both platforms.

No unavailable measurement may be converted to zero or treated as a pass.
