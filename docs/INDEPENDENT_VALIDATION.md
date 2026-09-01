# Independent validation evidence

Phase 2 requires external evidence for claims that Loop cannot prove with its
own parser, renderer, or preflight implementation. The qualification helper
`scripts/qualification/run_independent_validators.py` discovers the validators
from `PATH`; it does not install or bundle them.

The claims are intentionally separate:

| Claim | Default command | Meaning |
| --- | --- | --- |
| `structural` | `qpdf --check {input}` | Independent parser/structural check |
| `signature` | `pdfsig {input}` | Signature inspection; a file with no signatures is incomplete |
| `standards` | `verapdf validate --format text {input}` | PDF/A or PDF/X conformance authority |

Example:

```text
python scripts/qualification/run_independent_validators.py \
  --input candidate.pdf \
  --output independent-validation.json \
  --candidate-sha <candidate-sha> \
  --claim structural --claim signature --claim standards
```

The output follows
[`independent-validation-evidence.schema.json`](schemas/independent-validation-evidence.schema.json).
Each invocation records the discovered executable, best-effort version,
expanded arguments, candidate byte count and SHA-256, exit status, duration,
bounded stdout/stderr, platform, and a normalized result. Missing tools,
timeouts, invocation failures, and absent signatures are `incomplete`; a
nonzero validator exit is `rejected`. `incomplete` never qualifies as PASS,
and self-only Loop checks do not satisfy this gate.

The conversion fixture triad remains the source-level oracle in
`loop-preflight/testdata/conversion/manifest.json`. Any real PDF added for a
platform qualification run must record provenance, license, digest, expected
validator result, and known limitations alongside the evidence artifact.
