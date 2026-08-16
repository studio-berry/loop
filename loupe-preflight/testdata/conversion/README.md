# Standard conversion fixtures

Synthetic, permission-cleared triad used by `UnitTestsStandardOracle`:

| Case | How it is built | Expected |
| --- | --- | --- |
| Already / safely convertible | Empty page + PDF/A-2b + mock validator exit 0 | Conversion may commit only after the independent validator passes |
| Deliberately unconvertible | Empty page + PDF/X-1a (unfixable font/structure blockers) | No conversion commit, no conformance marker |
| Oracle missing / mismatch | Empty `validator_program`, or mock exit 1 | Conversion **error**; never self-certified PASS |

veraPDF is **not** bundled. The CI oracle lane skips when `verapdf` is not on
`PATH`. A present veraPDF that disagrees with Loupe is an error, not a pass.
