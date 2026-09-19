# Certified preflight

Loop certification records that one exact document revision passed one exact effective preflight
profile. The certificate is standalone JSON and certification never writes provenance into the PDF.

## Issue a certificate

Run preflight with an output path:

```text
PdfTool preflight --profile profile.json --certify certificate.json document.pdf
```

Certification fails closed. Loop refuses issuance when inspection is incomplete, any check was
skipped or unsupported, any processing budget was exceeded, an error finding lacks an active
decision, the effective profile is provisional, or the document bytes changed after the run.
Warnings and actively waived errors remain certifiable; waived errors are counted and their
decision identities are retained in the certificate.

The certificate is prepared as a temporary file, the `CertificateIssued` event is appended to the
canonical operation-history chain, and only then is the standalone JSON committed. If publication
fails after issuance is recorded, Loop appends `CertificateInvalidated` rather than silently
leaving the ledger in a valid state.

## Verify a certificate

Run:

```text
PdfTool verify-certificate certificate.json document.pdf
```

The command returns zero only when the document digest, retained certificate payload, issuance
event, audit-chain head, and covering decisions all verify. It returns non-zero for a changed
document, a broken or edited chain, a stale decision, a missing issuance record, or an explicitly
invalidated certificate.

Any byte change invalidates the prior certificate, including a repair that improves the document.
Repairs append `FixApplied` events and, when they replace a certified revision in the same history
sidecar, retain the old certificate and append `CertificateInvalidated`.

## Editor states

The Preflight workspace exposes a separate certified-preflight indicator with exactly three
presentation states:

- **Certified** — the retained certificate verifies for the current file bytes and history.
- **Not certified** — no certified-preflight record exists for the document.
- **Certificate invalid** — a certificate exists but the document, decision state, or audit chain no
  longer verifies. Unsaved document changes are also shown as invalid rather than as never certified.

The ordinary preflight result remains separate from certification. A clean current run is not the
same thing as an issued certificate.

## Storage and trust limits

Audit and certificate provenance use the append-only `PDFOperationHistoryEvent` chain in
`<document>.loop-history`; there is no parallel audit log. Both PdfTool and Editor preflight runs
append `DocumentOpened` / `PreflightRun` events through the same Core audit writer. The issuance event retains the
standalone certificate payload so invalid certificates remain inspectable instead of being deleted.

The chain is tamper-evident, not tamper-proof. A person with write access to the complete sidecar can
replace the entire chain. Operator identity is attribution, not authentication.

Certified preflight is not a digital signature. It provides no PKI trust, third-party attestation,
or legal non-repudiation, and it is deliberately separate from Loop's PDF signature and certificate
store machinery.
