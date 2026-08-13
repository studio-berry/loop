# Canonical preflight verdict

Loupe reduces every normalized `PreflightResult` through
`pdf::reducePreflightVerdict()` in Core. The reducer is the only authority for
the operator-facing outcome; the legacy `pass` boolean is emitted only as a
derived compatibility field.

The terminal states are:

| State | Meaning | PdfTool exit code |
| --- | --- | ---: |
| `pass` | Inspection completed and no unwaived blocking finding remains | 0 |
| `fail` | Inspection established at least one blocking finding | 1 |
| `incomplete` | Required evidence was not inspected, including budget exhaustion | 8 |
| `error` | The document, profile, or engine operation failed | 9 |

Reports carry a `verdict` object with the state, machine-readable
`reason_code`, human-readable reason, and the blocking/waived stable finding
IDs. A budget finding is evidence that inspection could not finish; it is not a
blocking finding by itself. This prevents a raster budget exhaustion with zero
findings from being reported as a clean pass.

Core, PdfTool, PageMaster export, repair postflight, and the Editor sidecar
consume this same contract. New surfaces must call the Core reducer or consume
the normalized `verdict` object; they must not infer status from
`errors.isEmpty()` or `findings.isEmpty()`.
