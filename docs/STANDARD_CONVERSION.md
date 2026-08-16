# PDF/X and PDF/A conversion

Loupe exposes standard conversion as the Core operation `standards-convert`.
PdfTool's `repair` command and PageMaster's headless export job call this same
operation; an Editor adapter can be added after the 0.1.1 GUI gate without
creating a second conversion implementation.

Supported targets are explicit: `PDF/X-1a:2001`, `PDF/X-3:2002`, `PDF/X-4`, and
`PDF/A-2b`. The selected target is recorded in the operation plan and report.
The report lists metadata, PDF version, output-intent, page-box, and optional
color-normalization changes before mutation.

Conversion is fail-closed. A CMYK ICC profile is required for PDF/X-1a and
PDF/X-3 normalization. Loupe does not claim that transparency was flattened,
fonts embedded, actions removed, or other unsupported constructs repaired when
the Core implementation cannot do so. Those findings remain blockers.

Every non-dry-run conversion requires an independent validator command. The
validator receives a temporary candidate through the `{input}` argument
placeholder. A zero exit status is necessary but not sufficient for PDF/X:
Loupe also runs its own postflight policy and commits only when both pass. For
PDF/A-2b, the external validator is the conformance authority; Loupe's own
metadata/output-intent work is not a PDF/A conformance claim.

Example:

```text
PdfTool repair input.pdf --operation standards-convert \
  --param target=PDF/X-4 \
  --param target_icc_base64=<base64-icc> \
  --param validator_program=verapdf \
  --param validator_arguments="validate --format text {input}" \
  --output output.pdf
```

No validator or Java runtime is bundled by default. Optional veraPDF/Temurin
packaging and licensing decisions are documented in
[`PACKAGING_LICENSING.md`](PACKAGING_LICENSING.md).

Independent validation is skip-if-missing in CI (`UnitTestsStandardOracle`
skips when `verapdf` is not on `PATH`). When a validator is configured, a
nonzero exit is a conversion **error**: no candidate is committed and Loupe
never self-certifies PASS. Mock always-pass / always-fail / missing-program
tests cover that fail-closed table without bundling a JRE.

Synthetic conversion fixtures and the already-conformant / convertible /
unconvertible triad are described in
[`loupe-preflight/testdata/conversion/README.md`](../loupe-preflight/testdata/conversion/README.md).
Renderer differentials for color and overprint live in
[`RENDERER_DIFFERENTIALS.md`](RENDERER_DIFFERENTIALS.md).

## Fixture triad

`loupe-preflight/testdata/conversion/manifest.json` names three cases:

| Kind | Meaning |
|------|---------|
| already-conformant | Structural stand-in; still requires an independent validator |
| safely-convertible | Metadata Loupe can rewrite when a validator is configured |
| deliberately-unconvertible | Oracle mismatch must remain ERROR, never PASS |

`UnitTestsConversionOracle` proves a missing oracle cannot self-certify and that
`/bin/false` (or a failing veraPDF) is ERROR. The veraPDF lane is skip-if-missing.
