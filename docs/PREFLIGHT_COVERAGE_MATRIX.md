# Preflight check catalog and GWG / PDF/X coverage

Loupe publishes a generated check catalog and a coverage matrix so a clean
preflight pass is read as **clean against this scope**, not as a Ghent Workgroup
certificate.

## Sources

- Registry: `PreflightEngine::registerBuiltInChecks()`
- Overlay: [`docs/preflight-check-catalog-overlay.json`](preflight-check-catalog-overlay.json)
- Generated catalog: [`docs/generated/preflight-check-catalog.json`](generated/preflight-check-catalog.json)

Regenerate after adding or renaming a check:

```text
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

`--check` fails when a registered check has no overlay entry, or an overlay
entry names a check that is not registered.

## Coverage values

Each check is `covered`, `partial` (limitation named in the overlay), or
`not_covered`. Process families follow GWG's sheetfed offset, web offset,
packaging, newspaper, and digital printing groups, plus Loupe's audited PDF/X
targets. See also [`PDFX_POLICY_MATRIX.md`](PDFX_POLICY_MATRIX.md).

## Claim

**Loupe does not claim formal GWG conformance.** The matrix is a measurement
and backlog tool. A report's `coverage_scope` object carries the same claim
with the enabled check ids for that run.

Uncovered classes currently include GWG 2022/2024 certificates, PDF/X-5,
PDF/VT, PDF/A-3 conversion claims, per-named-colorant ink limits beyond
inventory, barcode/slug/Braille validation, and imposition.
