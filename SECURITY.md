# Security Policy

## Supported versions

Loupe-PDF is released from the `master` branch of [mberrys/Loupe-pdf](https://github.com/mberrys/Loupe-pdf).

| Version | Supported |
| ------- | --------- |
| 0.1.x (current `PDF4QT_VERSION`) | Yes — security fixes land here |
| Older fork tags / unreleased branches | Best effort only |

Upstream PDF4QT releases are not covered by this policy; report upstream issues to [JakubMelka/PDF4QT](https://github.com/JakubMelka/PDF4QT) when they are not Loupe-specific.

## Reporting a vulnerability

Please report security issues privately:

1. Open a **private** vulnerability report on GitHub for `mberrys/Loupe-pdf` (Security advisories), **or**
2. Email the maintainer listed on the GitHub profile for this fork.

Include:

- Affected binary/surface (PdfTool, Editor, PageMaster, library)
- Loupe version / commit hash
- Minimal PDF or steps to reproduce
- Crash / DoS / info-disclosure impact

You should receive an acknowledgement within **7 days**. We aim to ship a fix or mitigation guidance within **30 days** for confirmed high-severity issues (crash/DoS on untrusted PDFs, path traversal, unsafe writes).

Please do **not** open a public GitHub issue for unfixed vulnerabilities.

## Scope notes

Highest-risk surfaces for this project:

- PDF parsers and stream filters (`Pdf4QtLibCore`)
- Image codecs (JBIG2, CCITT, DCT)
- Attachment / launch / URI handlers
- Atomic write / export paths (PageMaster, PdfTool)

Fuzz harnesses live under `Fuzz/` and `.github/workflows/fuzz.yml`. Crash reporting may be sent via Sentry when `SENTRY_DSN` / `PDF4QT_ENABLE_SENTRY` is configured — treat that as operational telemetry, not a substitute for private disclosure. Sentry is configured with `send_default_pii` disabled; see `docs/PRODUCTION_RUNBOOK.md` for opt-in guidance.

## Logging and diagnostics bundles

PdfTool and the Editor write a rotating log file (`pdf::PDFLogSession`, 2 MiB cap, 3 files kept) and can build a support bundle on request (`PdfTool diagnostics`; Editor → Help → Collect Diagnostics…). Both are enforced in code, not just documented:

- Every log line is passed through `pdf::PDFLogScrubber` **inside the message handler**, before it is written — no call site can skip scrubbing. Scrubbed: the home and temp directories, the login name, the host name, Windows/POSIX/UNC paths (basename dropped, extension kept), email addresses, and IPv4/IPv6 literals.
- A diagnostics bundle contains `manifest.json` (with a SHA-256 per file), `system-info.json`, `plugins.json` (Editor only), and the already-scrubbed log files (re-scrubbed defensively on copy). Arbitrary settings, recent files, environment variables, and command-line arguments are excluded.
- A bundle **never** contains a PDF, document content, or a crash minidump. It is staged and atomically published as a plain directory the user can inspect before sending to anyone; on any write or publish failure, no partial bundle is left on disk.
- This scrubbing guarantee applies **only** to the log/diagnostics path. It is a separate, stronger property than Sentry's: as noted above, Sentry's `send_default_pii` covers SDK-attached identifiers only — a crash minidump captured by crashpad can still contain PDF content and file paths, and nothing in the Sentry SDK can scrub that (see R-008 in `docs/V1_RELEASE_READINESS.md`). Do not conflate the two, and do not extend either guarantee's wording without changing the code that enforces it.

For V1 launch readiness and operational procedures, see `docs/V1_RELEASE_READINESS.md` and `docs/PRODUCTION_RUNBOOK.md`.
