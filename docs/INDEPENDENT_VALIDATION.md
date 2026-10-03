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
| `standards` | `verapdf --format xml --flavour 2b {input}` | PDF/A-2b only; parsed compliance report |

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
Each invocation records the discovered executable, recognized version,
expanded arguments, candidate byte count and SHA-256, exit status, duration,
bounded stdout/stderr, platform, and a normalized result. Missing tools,
timeouts, invocation failures, and absent signatures are `incomplete`; a
explicit negative report is `rejected`. qpdf exit 2 rejects structural integrity; other invocation failures without a recognized report remain incomplete. `incomplete` never qualifies as PASS,
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

The historical conversion fixture triad remains source scaffolding in
`loop-preflight/testdata/conversion/manifest.json`. Any real PDF added for a
platform qualification run must record provenance, license, digest, expected
validator result, and known limitations alongside the evidence artifact.

## Qualification contract v2

[`independent-claims.json`](independent-claims.json) inventories conversion targets,
recognized unsupported conformance declarations, production surfaces, fixture
lineage, tool versions and limits. The source gate checks coverage and fixture
identities; it does not certify a release. veraPDF's [documented profiles](https://docs.verapdf.org/cli/validation/)
cover PDF/A and PDF/UA. PDF/X preview and inspection remain available, but every
PDF/X standards-conversion request is refused before publication because this
scope supplies no qualified PDF/X oracle.

Evidence v2 requires the full 40-character source SHA, target and scope, exact
artifact digest, validator identity/version, command, raw-report digest,
platform and explicit limitations. Historical v1 records remain readable by
the schema and cannot satisfy the v2 qualification gate. A positive report must
name the expected input and PDF/A-2b profile, contain consistent rule/check and
batch totals, and complete normally. An unknown executable or report format,
missing version, timeout, cancellation or unavailable coverage stays incomplete.

Core separates `PDFStandardConversion::prepare()` from `validateArtifact()`.
Preparation never sets `independentValidationPassed`. Repair transactions,
Action List output adapters (including Editor), and PageMaster finish all edits,
serialize once, validate those bytes, and publish those same bytes atomically.
Core checks validator input identity and publication readback. Any subsequent
byte mutation requires new validation; evidence is attached to the recorded
digest, never transferable to another serialization. Existing governed
publication and sign-off gates still apply. Nested Core validation results
identify artifact bytes; the standalone reproduction packet supplies source and
platform provenance, so a nested result alone is not a release qualification.

`standards-convert` is operation version 2. Migrate existing recipe parameters by
adding `validation_contract: 2`; no prior recipe can silently opt into the new
publication contract. The validator arguments are fixed to XML/PDF/A-2b. Other
repair operations keep their existing save policies. PDF/A preparation is
bounded; real validation may reject a document it cannot safely convert.

`pdfsig` reports are parsed per signature. Signed ranges identify the end of the
signed revision; integrity, total-document coverage, and certificate validation
are recorded separately. A valid signature with unknown certificate trust can
prove signed-revision integrity only. It does not establish certificate trust,
coverage of a later append, or long-term validation. The existing signed base
fixture and its incremental append are reused without signing new documents.

## Reproduction packets

The manual `independent-qualification.yml` workflow reuses the existing Windows
and Linux build lanes for the same workflow source SHA. Dispatch requires a
reviewed immutable HTTPS oracle ZIP URL and SHA-256 for each platform. There are
no default or floating tool distributions. Local build configuration, dependency
provisioning and hosted dispatch retain the repository approval boundary.

Each ZIP contains external open-source tools and their runtime dependencies, a
platform-compatible Pillow wheel, pinned default Ghostscript ICC profiles, and
`oracles.json` with this shape (replace placeholders with reviewed identities):

```json
{
  "schema": "loop.oracle-distribution", "schema_version": 1,
  "tools": {
    "qpdf": {"path": "bin/qpdf", "sha256": "<64 hex>", "version": "11.x or 12.x", "upstream": "qpdf/qpdf", "license": "Apache-2.0"},
    "pdfsig": {"path": "bin/pdfsig", "sha256": "<64 hex>", "version": "23.x to 29.x", "upstream": "Poppler", "license": "GPL-2.0-or-later"},
    "verapdf": {"path": "bin/verapdf", "sha256": "<64 hex>", "version": "1.28.x", "upstream": "veraPDF", "license": "GPL-3.0-or-later / MPL-2.0"},
    "ghostscript": {"path": "bin/gs", "sha256": "<64 hex>", "version": "10.x", "upstream": "ArtifexSoftware/ghostpdl", "license": "AGPL-3.0-or-later"}
  },
  "profiles": {
    "rgb": {"path": "icc/default_rgb.icc", "sha256": "<64 hex>"},
    "cmyk": {"path": "icc/default_cmyk.icc", "sha256": "<64 hex>"},
    "gray": {"path": "icc/default_gray.icc", "sha256": "<64 hex>"}
  },
  "pillow_wheel": {"path": "python/Pillow-platform.whl", "sha256": "<64 hex>"}
}
```

Windows uses executable or `.bat` paths. The installer checks the ZIP digest,
member paths, required tools and member digests before exposing their directories
on PATH. Missing distributions or unsupported tool versions cannot qualify.
The packet retains Core output PDFs, full validator reports, native test logs,
commands, executable digests, fixed-region rendering measurements, separation
planes and delta images, distribution identity, corpus revision and run URL.

Run the helper only against a configured build of an unchanged source checkout:

```text
python -m scripts.qualification.run_independent_packet --source-sha <full SHA> --build <existing build> --output <new packet directory> --distribution <verified oracle directory> --distribution-sha256 <ZIP SHA-256> --run-url <run URL>
python -m scripts.qualification.verify_independent_packets --source-sha <full SHA> <Linux packet> <Windows packet>
```

The aggregate requires both platforms, identical source/corpus/inventory
identities, all mandatory native tests, complete independent fixture coverage,
and matching retained artifact/report digests. An expected negative is a
successful rejection test; its PDF remains rejected. Render mismatches remain
failed qualification. This work does not grant Core release admission, which
remains with issue #21. Packet integrity proves retained byte identity and
coverage; trust in the build/run origin still comes from the workflow provenance.
