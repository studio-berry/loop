# Loupe PDF/X policy matrix

The PDF/X layer is an optional policy pack in `PreflightEngine`. It composes
existing inspection primitives and emits the same normalized findings consumed
by PdfTool and the Editor plugin.

## Audited targets

| Target | Loupe policy revision | Policy-specific behavior |
| --- | --- | --- |
| PDF/X-1a:2001 | `1` | Requires PDF 1.3, trailer IDs, PDF/X metadata, identified output condition, a GTS_PDFX output intent with a valid matching ICC profile, inherited TrimBox and BleedBox, embedded fonts, no DeviceRGB paint, no live transparency, inspectable overprint flags, and no active document/annotation actions. |
| PDF/X-4 | `1` | Requires the same structural and output-intent evidence, permits PDF 1.4 or later, DeviceRGB content, and live transparency subject to the existing risk inspection. |
| PDF/X-3:2002 | `1` | Requires the shared PDF/X structural policy, PDF 1.3, an identified GTS_PDFX output intent, embedded fonts, TrimBox/BleedBox, inspectable overprint, no live transparency, and no active actions. |

## Stable rule registry

The registry is intentionally auditable and versioned in
`LoupeLibCore/sources/preflightengine.cpp`:

`pdfx.document.version`, `pdfx.document.trailer-id`,
`pdfx.document.encryption`, `pdfx.metadata.identification`,
`pdfx.output-intent.present`, `pdfx.output-intent.identity`,
`pdfx.output-intent.subtype`,
`pdfx.output-intent.profile`, `pdfx.output-intent.profile-space`,
`pdfx.page.trim-box`, `pdfx.page.bleed-box`, `pdfx.font.embedded`,
`pdfx.color.device-rgb`, `pdfx.transparency.allowed`,
`pdfx.overprint.inspectable`, and `pdfx.annotation.forbidden-action`.

Each rule returns `passed`, `failed`, `not-inspected`, or `not-applicable` plus
JSON evidence. The generated coverage matrix (`preflight_coverage` in
[`generated/architecture-catalog.json`](generated/architecture-catalog.json))
lists which Core checks feed the Evidence Graph families and which remaining
checks are explicit coverage holes. Mandatory failures reduce the result to `non-conformant`;
mandatory missing evidence reduces it to `incomplete`; only an all-pass
mandatory set can produce `conformant`. A definite failure takes precedence
over unrelated incomplete rules.

## Reuse and safety boundaries

- Output intent and ICC checks reuse the catalog output-intent model and the
  existing LittleCMS validation path.
- Page-box rules consume normalized inherited `PDFPage` boxes.
- Font and color rules call the existing recursive resource/font and content
  color inspection paths; they do not parse user-facing messages.
- Transparency and action rules inspect structure only and never execute active
  content. Overprint evidence records that Output Preview remains the
  authoritative separation/overprint renderer.
- The policy run is read-only and is contained by the same exception boundary
  as ordinary preflight. Any mandatory rule that throws becomes
  `not-inspected`, never `passed`.

The `pdfx` profile object and report field are additive. Profiles without
`pdfx` retain the existing engine behavior and report shape.
