# Session 08 — residue sweep handoff

**Current state:** candidate graph recorded; acceptance remains blocked by the
Session 07 package boundary and final native/hosted qualification.

## Scope

Session 08 removes deleted Widgets-era product identities from maintained build,
packaging, CI, current documentation, and desktop-entry paths. Historical
provenance and negative contract fixtures remain explicitly non-authoritative.

## Implementation

- Removed retired secondary desktop entries and icons.
- Reduced the WiX project to the current x64 LoupeEditor/PdfTool product boundary.
- Updated current workspace, platform, scheduler, recovery, accessibility, and
  repository-map documentation to use the Quick/Core graph.
- Added `scripts/ci/check_phase5_residue.py`, wired into source-integrity CI,
  with negative fixture coverage.

## Verification record

Candidate `cdx/0.2.0-p5session7` at
`d099622a0abee38e98c9378cbfd9763f4233b8a8` was checked from a fresh worktree.
The residue gate, generated Phase 5 evidence, source integrity, product/shell
contracts, architecture/catalog checks, and the full `scripts/ci` contract
suite pass (183 tests, 1 intentional skip). The hosted Supply Chain Policy run
`33462071106` is green for this SHA.

This is source/static proof only. No accepted Linux AppImage or Windows MSI
package pair, clean-machine install/smoke transcript, native release build, or
full hosted release-gate result exists for this candidate. Session 08 therefore
remains blocked and must not be marked Done from the static result alone.

The final record must be completed on the pushed branch with the exact commit
SHA and command output for:

- Session 07 Linux AppImage and Windows MSI clean-machine evidence;
- `scripts/ci/check_phase5_residue.py` and its unit fixtures;
- source integrity, shell/product contracts, Phase 5 contracts, and focused CI;
- clean-checkout release-profile validation and required hosted CI lanes.

Session 08 must not be marked complete until Session 07’s package evidence is
accepted and the clean-checkout result is green. Package, local, hosted, and
release evidence remain separate acceptance records.

## Next-session entry condition

Session 09 may start only from this branch’s accepted exact SHA after Issues
LOUPE-25, LOUPE-26, and LOUPE-27 and the Session 08 exit gate are updated from
the final evidence record.
