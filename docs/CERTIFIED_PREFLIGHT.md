# Certified preflight

Loop certification records that one document revision passed one effective preflight profile.
The certificate is standalone JSON. It never changes the PDF.

## Issue a certificate

Run preflight with an output path:

```text
PdfTool preflight --profile profile.json --certify certificate.json document.pdf
```

Loop refuses certification when inspection is incomplete, a check was skipped or budget-limited, an
error finding lacks an active decision, or the effective profile is provisional. Active waivers stay
visible in the certificate count.

## Verify a certificate

Run:

```text
PdfTool verify-certificate certificate.json document.pdf
```

The command returns zero only when the document digest and the operation-history chain match the
certificate. It returns a non-zero result for a changed document, a broken chain, or a stale decision.

## Trust limits

The certificate uses the append-only operation-history sidecar at `<document>.loop-history`. The
history is tamper-evident, not tamper-proof. A person with write access to the complete sidecar can
replace the chain. The operator identity is attribution, not authentication.

This feature is not a digital signature. It does not provide PKI trust or legal non-repudiation.
Loop's PDF signature and certificate-store features remain separate.
