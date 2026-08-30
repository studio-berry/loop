# Issue #264: exact release candidate dossier

`docs/schemas/release-candidate-dossier.schema.json` defines the shape. This
file records what is already true on `dev`/`0.2.0-minor-fixes` and what a
dossier still has to assemble that nothing currently automates.

## Already true

- `CreateReleaseDraft.yml` refuses to cut a release unless the Linux AppImage
  and Windows MSI package job runs it finds both have `headSha ==
  ${GITHUB_SHA}` (the commit the release workflow itself is running on) --
  see its two `if [ "$run_sha" != "${GITHUB_SHA}" ]` guards. A rerun cannot
  silently substitute a different source SHA into either artifact without
  the workflow failing first (issue #264 AC4).
- The same workflow publishes `SHA256SUMS.txt` over every artifact it ships.
- `scripts/agent/check-change.py` already produces a structured JSON report
  (`source_integrity`, `architecture_catalog`, `policy_adapters`, per-file
  `format`, `build:*`, `focused_tests`, `clang_tidy`) keyed to an exact
  `head_sha`/`comparison_base_sha` pair. This is a natural input to the
  dossier's `checks.source_integrity`, `checks.architecture_documentation_truth`,
  and part of `checks.build_and_test_{windows,linux}` -- it is not itself the
  full dossier.
- `docs/0.2.0-closeout-matrix.md` already tracks most of the dossier's other
  required-evidence categories as gates: T-03 (independent validation) →
  `checks.developer_and_release_manifest`-adjacent evidence, P-01/P-02
  (package/supply-chain) → `checks.package_install_launch_smoke` and
  `checks.supply_chain_policy`, E-01/E-02/E-03 → the dossier's own existence,
  audit, and promotion. That matrix is the source of truth for what state
  each gate is in; this doc does not duplicate it.

## Not yet true -- no dossier has ever been assembled

Nothing currently:

- collects those scattered pieces of evidence into one `candidate_sha`-keyed
  JSON document matching the schema;
- computes or attaches an SBOM or a build-provenance attestation per
  artifact (the schema's `artifacts[].sbom`/`attestation` fields);
- runs the bounded `Fuzz/`/`build-fuzz-docker/` regression session as a
  release-gating check rather than an ad hoc developer run;
- produces the issue-acceptance crosswalk (every issue targeted at this
  release, mapped to pass/fail/waived evidence);
- performs E-02's "no-fix audit" (an independent pass that touches nothing,
  after implementation is frozen at `candidate_sha`);
- performs E-03's promotion step (tag and `stable` merge matching the exact
  qualified `candidate_sha`, never a later commit).

## Why this session did not build the assembly automation

Assembling and publishing a dossier means running the hosted Windows/Linux
Release Gate, computing real SBOMs/attestations, and running the fuzz
session -- all of which require a live CI run and a real build, neither
available in this sandbox. Per this repository's agent policy, packaging,
signing, and credential-adjacent operations require approval and are outside
the "format/build/test the tree you have" autonomous budget. Wiring a wrong
release-gating script in blind, with no way to run it, risks breaking actual
releases -- worse than leaving the gap explicit.

This session added the schema above (a pure documentation artifact,
zero execution risk) so the follow-up work has a validated target shape
instead of starting from the issue's inline JSON sketch.
