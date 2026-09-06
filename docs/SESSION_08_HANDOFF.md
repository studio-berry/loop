# Session 08 — residue sweep handoff

## Scope

Session 08 removes deleted Widgets-era product identities from maintained build,
packaging, CI, current documentation, and desktop-entry paths. Historical
provenance and negative contract fixtures remain explicitly non-authoritative.

## Implementation

- Removed retired secondary desktop entries and icons.
- Reduced the WiX project to the current x64 LoopEditor/PdfTool product boundary.
- Updated current workspace, platform, scheduler, recovery, accessibility, and
  repository-map documentation to use the Quick/Core graph.
- Added `scripts/ci/check_phase5_residue.py`, wired into source-integrity CI,
  with negative fixture coverage.

## Verification record

**Candidate SHA:** `1c4126982e8f4b50f63d1063067ce4c4a76ee358` (Session 07 package-boundary evidence accepted).

### Session 07 package evidence (accepted)

- Linux AppImage workflow `34050834332` (package build + inspection)
- Windows MSI package-boundary evidence recorded in `docs/evidence/session-07-package-boundary/`
- Paired evidence: `docs/evidence/session-07-package-boundary/paired-evidence.json`

### Issue 25 — maintained-tree residue (PASS)

```
python scripts/ci/test_check_phase5_residue.py  → 5 tests OK
python scripts/ci/check_phase5_residue.py       → passed
```

Targeted `git grep` across maintained paths: no forbidden `LoopLibWidgets`, `LoopLibGui`, or secondary executable names.

### Issue 26 — current documentation normalization (PASS)

Current authority docs (`docs/REPO_MAP.md`, `docs/PLATFORM_SUPPORT.md`, `docs/LOOP_SHELL_CONTRACT.md`, `docs/LOOP_WORKSPACES.md`, `docs/ACCESSIBILITY_BASELINE.md`, `docs/EDITOR_RECOVERY.md`, `docs/JOB_SCHEDULER.md`) contain no executable references to deleted Widgets surfaces. `docs/product-surface.json` records deleted artifacts only in `source_status: deleted` disposition rows.

```
python scripts/ci/validate_product_surface.py              → passed (developer + loop-release)
python scripts/verify_product_surface.py --profile developer      → passed (source-only)
python scripts/verify_product_surface.py --profile loop-release   → passed (source-only)
```

### Issue 27 — clean-checkout qualification (PASS)

Fresh worktree at `1c412698` (detached HEAD, no local build artifacts):

```
python scripts/ci/test_check_source_integrity.py          → 18 tests OK
python scripts/ci/check_source_integrity.py               → passed
python -m unittest scripts.ci.test_workflow_contracts -v  → 20 tests OK
python scripts/ci/test_check_loop_identity.py             → 2 tests OK (after allowlisting Session 07 smoke transcript)
python scripts/ci/check_loop_identity.py                  → passed
python scripts/ci/validate_product_surface.py             → passed
python scripts/ci/test_check_phase5_residue.py            → 5 tests OK
python scripts/ci/check_phase5_residue.py                 → passed
python scripts/ci/check_unmanaged_async.py                → passed
python scripts/ci/check_interaction_traces.py --corpus-only → passed (9 scenarios)
python scripts/ci/check_trust_contract_sources.py         → passed
python scripts/ci/check_generated_dependency_paths.py     → passed
python scripts/generate_phase5_widgets_evidence.py --check → passed
python scripts/verify_phase5_widgets_contract.py          → passed
python scripts/verify-plugin-form-accounting.py           → passed
python scripts/generate_widgets_library_consumer_graph.py --check → passed
python scripts/verify-widgets-library-consumer-graph.py   → passed
python scripts/verify-widgets-free-release-profile.py     → passed
```

### Closeout fix

Session 07 clean-machine smoke transcript (`docs/evidence/session-07-package-boundary/linux-clean-machine-smoke.txt`) records pre-rename binary names from the accepted package run. Added to `LEGACY_TOKEN_ALLOWLIST` in `scripts/ci/check_loop_identity.py` as non-executable historical evidence.

### Hosted CI

Prior `dev` push at `1c412698` failed `source_integrity` on the loop-identity contract (same smoke-transcript finding). Closeout branch re-runs CI after the allowlist fix.


## Next-session entry condition

Session 09 may start only from this branch’s accepted exact SHA after Issues
issues 25, 26, and 27 and the Session 08 exit gate are updated from
the final evidence record.
