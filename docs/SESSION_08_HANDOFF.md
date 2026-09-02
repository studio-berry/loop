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
issues 25, 26, and 27 and the Session 08 exit gate are updated from
the final evidence record.
