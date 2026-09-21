# Portable proof-of-preflight evidence bundle

Issue #589 gives Loop a production handoff: one self-contained, integrity-manifested
directory a shop can hand to a customer, a press, or an auditor, and that can be verified
without Loop's database, sidecar, or application state.

**The bundle is not a second source of truth.** The internal certified-preflight state, the
canonical `PDFOperationHistoryStore` chain, and the document sidecar remain authoritative.
The bundle is an export of one revision's place in that state; `manifest.json` says so in its
`authority` block, and no Loop surface reads a bundle back as state.

Cross-vendor preflight audit trails are an industry direction (GWG's Universal Proof of
Preflight work). **Loop claims no GWG conformance** — see
[`PREFLIGHT_COVERAGE_MATRIX.md`](PREFLIGHT_COVERAGE_MATRIX.md). The bundle publishes the
coverage scope that was actually evaluated so a reader can size the claim themselves.

## Export

```text
PdfTool export-evidence-bundle artwork.pdf \
  --report preflight-report.json \
  --certificate certificate.json \
  --output handoff-bundle
```

| Option | Required | Meaning |
| --- | --- | --- |
| `<document.pdf>` | yes | The exact revision the bundle describes. Only its digest is retained. |
| `--report <file>` | yes | The canonical preflight report for that revision. |
| `--output <dir>` | yes | Bundle directory. Created when absent; refused unless empty. |
| `--certificate <file>` | no | Retained certified-preflight JSON ([`CERTIFIED_PREFLIGHT.md`](CERTIFIED_PREFLIGHT.md)). |
| `--sign-off <file>` | no | Governed publication sign-off (`loop.governed-sign-off`, [`GOVERNED_EXECUTION.md`](GOVERNED_EXECUTION.md)). |
| `--artifact <file>` | no | Corrected output artifact whose identity the bundle declares. |

The command is JSON-first: it emits exactly one PdfTool result envelope, and supplying a
different `--console-format` is an invalid invocation.

Export fails closed — `bundle.refused` (`1 findings`) — when the report does not carry the
revision digest of the supplied bytes, when it carries no effective profile digest or
coverage scope, when the supplied certificate binds a different revision, profile, or
report, when the sign-off record binds a different source revision or profile, when the
declared artifact is not the artifact the sign-off published, when the document has no
operation-history sidecar, or when the supplied chain does not verify from sequence 1.

## Verify

```text
PdfTool verify-evidence-bundle handoff-bundle
```

`verify-evidence-bundle` reads only the bundle directory: no document, no sidecar, no
network. Exit code `0` and `data.verification.valid: true` mean the bundle verifies; any
attributable finding returns `1 findings` with `data.verification.findings[]`.

Stable finding codes (`code` in `data.verification.findings[]`), each naming the member it
belongs to:

| Code | Meaning |
| --- | --- |
| `bundle.manifest-missing` | The directory carries no readable `manifest.json`. |
| `bundle.schema-unsupported` | Unknown bundle schema or version. |
| `bundle.document-digest-invalid` | The manifest declares no usable revision digest. |
| `bundle.document-digest-mismatch` | The report and the manifest describe different revisions. |
| `bundle.profile-digest-missing` / `bundle.profile-digest-mismatch` | The manifest carries no effective profile digest, or it disagrees with the report's. |
| `bundle.coverage-scope-missing` / `bundle.coverage-scope-mismatch` | The manifest's coverage scope is absent or disagrees with the report's. |
| `bundle.decision-mismatch` | The manifest's decisions and the report's decisions name different findings. |
| `bundle.source-path-present` | The exported report still carries the source path key. |
| `bundle.report-binding-invalid` | The manifest declares an unknown certificate binding. |
| `member.missing` | A declared member is absent. |
| `member.undeclared` | The directory carries a file the manifest does not declare. |
| `member.name-invalid` | A declared member name is not a plain bundle-local name. |
| `member.size-mismatch` / `member.digest-mismatch` | A member's bytes disagree with the manifest. |
| `member.invalid-json` | A member is not the JSON object its role requires. |
| `report.member-mismatch` | `manifest.report.member_sha256` disagrees with the report member. |
| `report.certificate-binding` | The exported report does not hash to the certificate's report digest (checked only when the binding is `exact`). |
| `certificate.identity-mismatch` | The certificate binds a different revision or profile than the manifest. |
| `certificate.manifest-mismatch` | The certificate member disagrees with the manifest's certificate record. |
| `certificate.chain-head-missing` | The certificate's issuance event is not in the exported history slice. |
| `certificate.decision-unresolved` | A decision the certificate covers cannot be re-identified from the exported decisions. |
| `decision.unreadable` | An exported decision is not a valid decision record. |
| `history.sequence-discontinuous` / `history.chain-broken` | The exported chain does not verify, named by sequence and event id. |
| `history.event-count-mismatch` / `history.head-mismatch` | The chain head or length disagrees with the manifest. |
| `signoff.identity-mismatch` | The sign-off record binds a different source or profile. |
| `output.identity-mismatch` | The declared output is not the artifact the sign-off published. |
| `rollback.raw-path` | A rollback reference carries a raw path. |
| `rollback.manifest-mismatch` | The rollback member disagrees with the manifest. |

## Bundle layout

| Member | Contents |
| --- | --- |
| `manifest.json` | Machine-readable manifest with a SHA-256 and byte count for every member below. Committed last, so a directory that carries a manifest is a complete bundle. |
| `report.json` | The preflight report: verdict, errors, warnings, checks with per-check scope restrictions, the coverage scope actually evaluated, and the decisions. |
| `certificate.json` | The retained certificate, verbatim, when the revision is certified. |
| `signoff.json` | The governed sign-off record, when a correction was published. |
| `history.json` | The complete operation-history slice for the revision, in sequence order, with its hash links. |
| `rollback-references.json` | Retained rollback points as digest-addressed references. |

`manifest.json` carries, at the top level, the fields the acceptance criteria name:

- `document` — `revision_digest` (SHA-256 of the exact revision bytes) and `byte_count`;
- `effective_profile` — `digest`, plus the resolver's `identity` and `resolution`
  provenance ([`PREFLIGHT_PROFILE_RESOLUTION.md`](PREFLIGHT_PROFILE_RESOLUTION.md));
- `coverage_scope` — the checks and scope the run actually evaluated;
- `certificate`, `sign_off`, `output` — the accepted identities, when present;
- `decisions` and `approvals` — accept/waive/override decisions and approval records with
  actor and timestamp;
- `history` — event count, sequence range, head event id and hash, chain mode, and a digest
  over the canonical chain;
- `rollback_references` — the same digest-addressed references as the member;
- `members[]` — every member with its `sha256` and `byte_count`.

## What the bundle deliberately does not carry

- **No absolute paths.** The exported report drops the source path key; free-form operator
  text (rationale, justification, plan summary) has any path-shaped substring replaced with
  `<path-omitted>`; rollback references carry the artifact digest instead of its path; and
  artifact identities drop the store's storage token.
- **No document bytes and no customer content beyond the report.** The document is
  identified by digest. There is no signature or PKI material in the bundle: it is
  signature-*ready* (every member is hashed and every identity is a digest), not signed.
- **No second chain.** `history.json` is a copy of the canonical slice, not a new ledger.

## Two properties a reader must not over-read

1. **The exported chain is path-redacted, so its hashes are recomputed.** The canonical
   chain hashes the report payload as stored, which includes the source path. The exported
   slice therefore cannot carry the canonical per-event hashes; the manifest records
   `history.chain_mode: "path-redacted"` and `history.canonical_chain_digest`, a digest over
   the canonical `(sequence, entryId, eventHash)` triples. Offline verification proves the
   *exported* chain is internally consistent and untouched since export, and binds the head
   event by id (which the certificate also binds). Proving the exported slice derives from
   the canonical sidecar needs that sidecar, and the canonical digest is what to compare.
2. **A certificate's report digest is not always re-derivable offline.**
   `manifest.report.certificate_binding` is `exact` when the exported report hashes to the
   certificate's `report_digest`, and `path-omitted` when the certificate hashed a report
   that still carried the source path. Verification recomputes the binding in the `exact`
   case (`report_binding_recomputable: true`) and reports it as not recomputable in the
   `path-omitted` case rather than pretending the digest covers the exported bytes. Export
   refuses outright when the certificate does not bind the supplied report at all.

## Independent validation

`scripts/qualification/run_independent_validators.py --evidence-bundle <dir>` consumes a bundle as
described in [`INDEPENDENT_VALIDATION.md`](INDEPENDENT_VALIDATION.md): it requires the
bundle to verify, binds the validator run to the manifest's document revision digest, and
records the bundle's identity in the evidence record.

## Limits

The bundle is tamper-*evident*, not tamper-*proof*: it is not a digital signature and gives
no PKI trust, third-party attestation, or non-repudiation. Anyone who can rewrite the whole
directory can rewrite the manifest with it. Its value is that every member, every identity,
and the exported chain are checkable by a documented command, offline, without Loop.
