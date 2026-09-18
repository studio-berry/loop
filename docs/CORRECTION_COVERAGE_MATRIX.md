# Correction-operation catalog and coverage matrix

Loop publishes a generated correction-operation catalog so operators and
automation authors can see what each registered repair operation changes, how it
saves output, and where it is available across surfaces.

## Sources

- Registry: `PDFRepairRegistry` built-ins in `LoopLibCore/sources/pdfrepairprimitives.cpp`
  and `LoopLibCore/sources/pdfproductionrepair.cpp`
- Overlay: [`docs/correction-operation-catalog-overlay.json`](correction-operation-catalog-overlay.json)
- Generated catalog: [`docs/generated/correction-operation-catalog.json`](generated/correction-operation-catalog.json)

Regenerate after adding, renaming, or reclassifying an operation:

```text
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

`--check` fails when a registered operation has no overlay entry, when an overlay
entry names an operation that is not registered, or when overlay revalidation
class does not match the impact declared in code.

Bidirectional coverage is also exercised by
[`scripts/ci/test_correction_operation_catalog.py`](../scripts/ci/test_correction_operation_catalog.py).

## Catalog fields

Each operation row combines registry-derived facts with the reviewed overlay:

| Field | Source |
| --- | --- |
| `id`, `version`, `implementation` | Parsed from `PDFRepairOperation` classes |
| `save_policy.mode`, signature invalidation, session reversibility | Parsed from `savePolicy()` |
| `impact`, `revalidation_class`, `requires_postflight` | Parsed from `impact()` and plan flags |
| `supports`, `produces`, `target_scopes`, `parameter_defaults` | Overlay |
| `save_policy.artifact_effect`, `reversibility`, `evidence_impact` | Overlay |
| `revalidation.description`, `surface_parity`, `limitations` | Overlay |

Save-mode semantics for the source artifact are defined once in the generated
catalog under `save_modes` and referenced per operation.

Target scopes describe the current implicit selector behaviour. The shared
selector AST tracked in GitHub #587 is not yet wired into repair plans; until it
lands, operations declare whole-document, page, resource, or production-geometry
scopes resolved during `analyze()`.

## Claim

**Loop does not claim that executing an operation makes a file press-ready.**
The catalog states what each correction can change and which checks should rerun
afterward. Operators still review the serialized candidate, diff, and postflight
verdict before publish.

See also [`REPAIR_OPERATIONS.md`](REPAIR_OPERATIONS.md) for the transaction
lifecycle and [`PREFLIGHT_COVERAGE_MATRIX.md`](PREFLIGHT_COVERAGE_MATRIX.md)
for the inspection-side twin.
