# Session 13 — package and licensing qualification handoff

## Scope

Session 13 implements final-artifact SBOM, third-party notices, LGPL relink
evidence tooling, and the package identity / clean-machine procedure for P-02
and P-01 SHA re-proof. Session 07 evidence on `b47c62b2…` does **not**
transfer.

## Baseline

| Field | Value |
| --- | --- |
| Branch | `dev` (merged PR #539) |
| Qualification `candidate_sha` | `ebde8661bff037e5cae2d37e3c2e3eae8b2ca6b5` |
| Package workflow dispatch | `source_sha=20f73c3a84bbbcb1d45fbcff8bddc90365f636b0` — [Linux run 34068347154](https://github.com/studio-berry/loop/actions/runs/34068347154), [Windows run 34068348127](https://github.com/studio-berry/loop/actions/runs/34068348127) |

## Implementation

### Issue 40 — final-artifact SBOM, notices, LGPL evidence

- `scripts/ci/package_licensing_common.py` — shared component/license mapping
- `scripts/ci/generate_package_sbom.py` — SPDX 2.3 from package-boundary evidence
- `scripts/ci/generate_package_third_party_notices.py` — notices from shipped payload
- `scripts/ci/run_qt_relink_test.sh` / `run_qt_relink_test.ps1` — LGPL relink proofs
- Package workflows extended to emit SBOM, notices, and relink transcripts into
  evidence artifacts

### Issue 41 — package identity and clean-machine lifecycle

- `docs/SESSION_13_PACKAGE_LICENSING.md` — exact-SHA dispatch, pairing, smoke, and
  clean-machine procedure (Ubuntu 24.04 required; Server 2022 deferred)
- Reuses `inspect_package_dependencies.py` and `compare_package_boundary_evidence.py`

### Issue 42 — evidence freeze and gate bookkeeping

- `docs/evidence/session-13-package-licensing/` — evidence home (status `incomplete`
  until hosted package builds on candidate SHA)
- `scripts/ci/collect_package_licensing_evidence.py` — manifest assembler
- Closeout matrix P-01/P-02 updated; `quick-runtime-manifest.json` tooling pointers

## Verification record

Local verifier stack:

```
python -m unittest scripts.ci.test_generate_package_licensing -v
python scripts/ci/test_generate_package_licensing.py
python scripts/verify-quick-runtime-contract.py
```

## Gate status (honest)

| Gate | State | Blocker |
| --- | --- | --- |
| P-02 final-artifact SBOM | **Open** | Hosted `Linux_AppImage` + `Windows_MSI` on candidate SHA |
| P-02 third-party notices | **Partial** | Artifact generator implemented; final-artifact proof pending |
| P-02 Qt relink | **Open** | Hosted relink transcripts not yet archived |
| P-01 package identity | **Open** | Must re-prove on candidate SHA (Session 07 `b47c62b2…` invalid) |
| P-01 clean-machine smoke | **Open** | Linux container + Windows hosted MSI smoke on candidate SHA |

## Hosted package build blockers

1. **Approval required** per AGENTS.md for hosted packaging workflow dispatch.
2. Dispatch both workflows with `source_sha=<candidate_sha>` after this branch merges
   or from the branch head for qualification.
3. Copy workflow evidence artifacts into `docs/evidence/session-13-package-licensing/`
   and run `collect_package_licensing_evidence.py`.
4. Windows Server 2022 pristine VM remains **deferred to 1.0** (non-blocking).

## Next gate

After hosted package evidence is frozen with `status: passed`, update
`quick-runtime-manifest.json` release_gates to `complete` and mark P-02
`acceptance verified` in the closeout matrix. Session 14 requires all lanes
green on the **same** `candidate_sha`.
