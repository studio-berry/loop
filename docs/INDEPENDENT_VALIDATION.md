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

## Consuming a proof-of-preflight bundle

A shop can hand an auditor a portable bundle instead of a candidate plus claims
([`PREFLIGHT_EVIDENCE_BUNDLE.md`](PREFLIGHT_EVIDENCE_BUNDLE.md)). The qualification helper consumes one
without Loop, from the bundle's own bytes:

```text
python scripts/qualification/run_independent_validators.py   --input candidate.pdf   --evidence-bundle handoff-bundle   --output independent-validation.json   --candidate-sha <candidate-sha>   --claim structural --claim signature
```

`--evidence-bundle` adds a `bundle` block to the evidence record and fails closed: `member-missing`,
`member-digest-mismatch` (with the offending member names), `member-undeclared`, or
`input-does-not-match-manifest` set the lane `rejected`; a bundle that is absent or has no manifest sets
it `incomplete`. A `passed` bundle block means every declared member hashed to its manifest digest, the
directory carried nothing else, and the candidate is exactly the revision the manifest declares — the
external validator's verdict is then bound to a revision identity the shop cannot restate. This
consumer deliberately does not re-implement Loop's identity checks (effective profile, coverage scope,
certificate binding, exported chain): run `PdfTool verify-evidence-bundle` for those.

The save-policy claim is produced by `UnitTestsIncrementalSave`: when
`LOOP_SAVE_POLICY_EVIDENCE_DIR` is set it writes
`incremental-with-signature.pdf` — a genuinely signed document that keeps the
original signed byte range across an incremental append — plus a JSON record of
the source and artifact digests. `reusable-linux.yml` runs `--claim structural
--claim signature` over that artifact and stores the evidence under
`docs/evidence/session-15-save-policy/`. A host without `qpdf` and `pdfsig`
records `reason_code: validator-not-installed` and stays `incomplete`.

The conversion fixture triad remains the source-level oracle in
`loop-preflight/testdata/conversion/manifest.json`. Any real PDF added for a
platform qualification run must record provenance, license, digest, expected
validator result, and known limitations alongside the evidence artifact.
