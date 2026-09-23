# Canonical JSON byte format (D1)

Loop digests identity-bearing JSON with one Core helper:

- `pdf::canonicalizeJson(QJsonValue)` — recursively sorts object keys
- `pdf::canonicalJson(QJsonValue)` — returns the canonical UTF-8 bytes
- Digests: `SHA-256` hex over those bytes (lowercase)

This is the **only** byte format allowed for plan digests, profile digests,
certificate/report digests, and other identity-bearing 0.3.0 envelopes that call
these helpers. Narrative docs must not invent a second encoding.

## Mapping to RFC 8785 / JCS

| Rule | Loop behavior |
| --- | --- |
| Object member order | Keys sorted as UTF-16 code-unit sequences (JCS §3.2.3). Non-ASCII and supplementary-character keys are unambiguous under this order. |
| Insignificant whitespace | None (`QJsonDocument::Compact`). |
| Arrays | Element order preserved; each element is canonicalized. |
| Strings | Qt Compact UTF-8 JSON string encoding. |
| Booleans / null | Standard JSON tokens. |
| Numbers | **Bounded exception:** Qt Compact number serialization, not full JCS number normalization. Golden vectors pin accepted forms (`1` vs `1.0` as Qt emits them). |
| Scalar document roots | **Bounded exception:** a non-object, non-array root is wrapped in a one-element array before Compact encoding so digesters always hash a JSON document. |
| Absent vs null | Absent keys remain absent. Explicit `null` values are retained and hashed. |
| Invalid input | Rejected at parse/validation boundaries. Canonicalization never repairs invalid JSON. |

## Platform parity

Linux and Windows must produce identical canonical bytes and digests for the
golden vectors under `UnitTests/testdata/canonical-json/`. The vectors are the
cross-platform proof; do not accept platform-local pretty-printing or locale
number formats.

## Consumers

Identity-bearing call sites include (non-exhaustive):

- `pdf::computeOperationPlanDigest`
- Preflight effective-profile digests (`canonicalPreflightJson` builds on the same
  key-order contract; see its module docs if it adds profile-specific rules)
- Governed revalidation report digests
- Certificate and evidence-bundle digests that call `canonicalJson`

Any digest that intentionally does **not** use this contract must be recorded as a
bounded exception beside the call site and in the related ADR.

## Proof

- ADR: [`docs/adr/adr-011-architecture-contracts-d1-d5.md`](adr/adr-011-architecture-contracts-d1-d5.md)
- Tests: `UnitTestsGovernedExecution` (`canonicalJson_*` cases)
- Fixtures: `UnitTests/testdata/canonical-json/`
