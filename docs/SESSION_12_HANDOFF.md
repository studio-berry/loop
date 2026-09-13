# Session 12 — Prove lifecycle model

## Candidate identity

| Field | Value |
| --- | --- |
| Branch | `cursor/session-12-lifecycle-qualification` |
| Baseline | `origin/dev` @ `6a55130c…` |
| Evidence | `docs/evidence/session-12-lifecycle/evidence.json` |

## Deliverables (Issues 37–39)

- **Issue 37:** Four-seed / 64-command corpus in `UnitTests/testdata/lifecycle/` with `manifest.json`, schema fields `observed_result` + `shrink_history`, `UnitTestsLifecycle` replay tests, `scripts/ci/check_lifecycle_corpus.py`.
- **Issue 38:** Delta-debugging `shrinkTrace()` plus three promoted minimized failure traces (`failure-*-minimized.json`).
- **Issue 39:** `crossPlatformCorpusReportIsStable` and frozen evidence; hosted Linux replay on merge SHA.

## Verification

```text
python scripts/ci/check_lifecycle_corpus.py
python scripts/agent/check-change.py --base origin/dev
cmake --build build --target UnitTestsLifecycle && ctest -R UnitTestsLifecycle --output-on-failure
```

Corpus static validation passes locally. Full C++ replay requires the vcpkg/Qt toolchain in CI.
