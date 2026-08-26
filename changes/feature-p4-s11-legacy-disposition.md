# Legacy surface disposition ledger for Phase 5 deletion

Category: internal
Audience: developers
Breaking-Change: no
Summary: Extend docs/loupe-shell.json as the single disposition authority for Phase 4 exit.
Add legacy_surface_disposition rows for all 48 tracked .ui forms (MIGRATE, CONSOLIDATE,
HEADLESS, RETIRE) and enrich all 12 plugin policies with owner, replacement_target,
required_test, evidence_artifact, and deletion_condition metadata including ActionListPlugin
and OcrPlugin. Extend docs/schemas/loupe-shell.schema.json and harden
scripts/verify-loupe-shell-contract.ps1 with fail-closed inventory checks. Add negative
coverage in scripts/ci/test_verify_loupe_shell_contract.py.
