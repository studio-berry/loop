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
- Extended `CURRENT_DOCS` machine enforcement to session handoffs, packaging,
  and CI contributor guidance.

## Verification record

**Post-residue candidate SHA:** recorded on the accepted closeout branch after
PR #533 merge. Qualification below was executed on closeout branch commits
starting from Session 07 acceptance at `1c412698`.

### Session 07 package evidence (accepted prerequisite)

- Linux AppImage workflow `34050834332` (package build + inspection)
- Windows MSI package-boundary evidence in `docs/evidence/session-07-package-boundary/`
- Paired evidence: `docs/evidence/session-07-package-boundary/paired-evidence.json`
- Recorded on `dev` at `1c412698`

### Issue 25 — maintained-tree residue (PASS)

Terminal post-package sweep on the closeout candidate:

```
python scripts/ci/test_check_phase5_residue.py  → 6 tests OK
python scripts/ci/check_phase5_residue.py       → passed
```

Targeted grep across maintained paths: no forbidden deleted Widgets surfaces remain.

### Issue 26 — current documentation normalization (PASS)

Machine-enforced current docs (`CURRENT_DOCS` in `check_phase5_residue.py`):

- `docs/ACCESSIBILITY_BASELINE.md`
- `docs/CI.md`
- `docs/EDITOR_RECOVERY.md`
- `docs/JOB_SCHEDULER.md`
- `docs/LOOP_SHELL_CONTRACT.md`
- `docs/LOOP_WORKSPACES.md`
- `docs/PACKAGING_LICENSING.md`
- `docs/PLATFORM_SUPPORT.md`
- `docs/REPO_MAP.md`
- `docs/SESSION_07_PACKAGE_BOUNDARY.md`
- `docs/SESSION_08_HANDOFF.md`

Additional architecture/product verification:

```
python scripts/ci/validate_product_surface.py              → passed
python scripts/verify_product_surface.py --profile developer      → passed (source-only)
python scripts/verify_product_surface.py --profile loop-release   → passed (source-only)
python scripts/verify-loop-shell-contract.py               → passed
python scripts/verify-plugin-form-accounting.py            → passed
python scripts/generate-architecture-catalogs.py --check   → passed
```

Historical ADRs, Phase 5 deletion handoffs, and migration checklists that mention
retired surfaces remain preserved with explicit non-authoritative framing.

### Issue 27 — clean-checkout qualification (PASS)

Fresh detached worktree (no local build artifacts) at the closeout candidate SHA:

```
python scripts/ci/test_check_source_integrity.py          → 18 tests OK
python scripts/ci/check_source_integrity.py               → passed
python -m unittest scripts.ci.test_workflow_contracts -v  → 20 tests OK
python scripts/ci/test_check_loop_identity.py             → 2 tests OK
python scripts/ci/check_loop_identity.py                  → passed
python scripts/ci/validate_product_surface.py             → passed
python scripts/ci/test_check_phase5_residue.py            → 6 tests OK
python scripts/ci/check_phase5_residue.py                 → passed
python scripts/ci/check_unmanaged_async.py                → passed
python scripts/ci/check_interaction_traces.py --corpus-only → passed
python scripts/ci/check_trust_contract_sources.py         → passed
python scripts/ci/check_generated_dependency_paths.py     → passed
python scripts/generate_phase5_widgets_evidence.py --check → passed
python scripts/verify_phase5_widgets_contract.py          → passed
python scripts/verify-plugin-form-accounting.py           → passed
python scripts/verify-loop-shell-contract.py              → passed
python scripts/generate_widgets_library_consumer_graph.py --check → passed
python scripts/verify-widgets-library-consumer-graph.py   → passed
python scripts/verify-widgets-free-release-profile.py     → passed
```

Closeout fixes on the candidate branch:

- Allowlist Session 07 clean-machine smoke transcript in `check_loop_identity.py`
  (historical evidence, non-executable)
- Extend `CURRENT_DOCS` for session handoffs and packaging guidance (Issue 26)

### Hosted CI (PR #533)

- `source_integrity` — PASS
- Supply Chain Policy — PASS
- Documentation truth (`architecture-docs`) — PASS
- `agent-fast` — required PR lane (see PR checks for exact run ID)

Prior `dev` push at `1c412698` failed `source_integrity` on the loop-identity
contract; closeout branch resolves that finding.

## Exit gate

- No maintained source, test, CI, packaging, script, or current operational
  document references a deleted Phase 5 Widgets surface.
- Clean-checkout source/static validation is green on the post-residue candidate SHA.
- Hosted PR CI (`source_integrity`, policy, architecture-docs, agent-fast) is the
  terminal qualification record for merge to `dev`.

## Next-session entry condition

Session 09 may start only from the accepted exact SHA after Issues 25, 26, and 27
and this Session 08 exit gate are recorded in Notion from the merged evidence.
