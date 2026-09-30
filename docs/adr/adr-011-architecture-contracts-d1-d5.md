# ADR-011: Close architecture contracts D1–D5 before 0.3.0 qualification

**Status:** accepted
**Implemented-at:** governed execution (#369/#370) + contract closure (#675)
**Last-verified:** 2026-09-22 @ ff7b76563119980390e1a4cd0737e904007e6fac
**Superseded-by:** none
**Date:** 2026-09-22
**Deciders:** Loop 0.3.0 release architecture (#638 / #675)

## Context

0.3.0 already ships governed planning, preview, approval, publication, revalidation,
and sign-off. Qualification still needs those behaviors pinned as **contracts** with
executable proof, not inferred from implementation shape. This ADR closes the five
decisions named in [#675](https://github.com/studio-berry/loop/issues/675).

## Decisions

### D1 — one canonical byte format

Identity-bearing JSON digests use `pdf::canonicalJson()` / `pdf::canonicalizeJson()`
in LoopLibCore. Semantics are documented in
[`docs/CANONICAL_JSON.md`](../CANONICAL_JSON.md).

- Object keys are sorted lexicographically as UTF-16 code units (RFC 8785 / JCS
  object-member order).
- Encoding is Qt `QJsonDocument::Compact` over the recursively sorted value.
- Scalar roots are wrapped in a one-element array before Compact encoding (Loop
  bounded exception from document-only JCS).
- Number serialization follows Qt Compact (bounded exception from full JCS number
  rules). Golden vectors pin the accepted bytes/digests.
- Absent keys stay absent; JSON `null` is retained.
- Invalid JSON is rejected at parse boundaries; digesters never invent keys.

### D2 — preview has no publication authority

Technical and visual previews may materialize an isolated candidate for review.
Only `pdf::publishGovernedArtifact()` may publish approved candidate bytes to a
destination. Approval binds `plan_digest`, `source_sha256`, and `candidate_sha256`.
Stale or revoked approval fails before mutation. Preflight decisions are never
operation approval.

### D3 — destination binding is separate from semantic plans

`pdf::computeOperationPlanDigest()` hashes schema kind/version, trusted source
SHA-256, merged save policy, and analyzed repair plans. Destination path and
overwrite/collision policy are execution inputs to the publish gateway and to
`PDFSaveRequest` / `PDFSafeFileWriter`, not semantic-plan identity. Changing only
the destination must not change the plan digest. Trusted-source / in-place
overwrite refusal remains fail-closed.

### D4 — publication and durable completion are separate states

Governed corrective output uses this explicit state model:

1. **staged** — analyzed plan and optional isolated candidate
2. **revalidated** — published bytes read back and evaluated (when a profile is present)
3. **published** — destination bytes committed through the governed write gateway
4. **durable completion** — sign-off certificate and provenance/history append

A published artifact without durable completion is observable (no valid
`loop.governed-sign-off` / certificate event) and must not be represented as signed
off. History/provenance append failure must not rewrite published artifact bytes.

### D5 — cross-surface output equality

For named operations, CLI, Quick/Editor, and headless surfaces must agree on
canonical **plan identity** and governed **output identity** fields under pinned
writer inputs (`plan_digest`, source/candidate/published digests, revalidation and
profile digests). Exact PDF writer byte identity is **not** part of the equality
contract when the writer emits clock- or random-derived fields; that weaker
contract is explicit and covers the disposition of [#656](https://github.com/studio-berry/loop/issues/656)
(fail-closed proofs must use structural/in-memory comparison or governed digests,
not independent re-serialization).

## Proof

| Decision | Primary proof |
| --- | --- |
| D1 | `UnitTestsGovernedExecution` golden vectors + `docs/CANONICAL_JSON.md` |
| D2 | `UnitTestsGovernedExecution` preview/publish negatives |
| D3 | `UnitTestsGovernedExecution` destination/plan separation + existing save-request refusals |
| D4 | `UnitTestsGovernedExecution` publish-without-sign-off + `docs/GOVERNED_EXECUTION.md` |
| D5 | `scripts/ci/check_governed_parity.py` + D5 disposition in this ADR / governed tests |

## Consequences

- Topic PRs that change identity digests must update golden vectors deliberately.
- #656 remains a separate bugfix for flaky bleed fail-closed assertions; this ADR
  only dispositions the equality contract those assertions were misusing.
- Exact-candidate qualification for #638 still requires assembled-SHA evidence after
  merge; this ADR supplies the architecture gate content, not the promotion transport.
