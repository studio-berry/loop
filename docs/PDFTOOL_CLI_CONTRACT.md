# PdfTool CLI Result Contract

**Status:** adopted (issue #16)
**PdfTool automation schema version:** `1` (outer envelope)

Every PdfTool command that runs in JSON mode emits **exactly one JSON document to
stdout**. That document is the PdfTool result envelope. It versions **PdfTool's
automation contract**, which is separate from any domain report it carries
(preflight, OCR). Do not merge the two schemas.

The machine-facing contract is: branch on `status`, `exit_code`, and diagnostic
`code` — never on translated `message` text.

## Envelope

```json
{
  "schema_version": 1,
  "command": "preflight",
  "version": "1.6.0.0",
  "status": "findings",
  "exit_code": 1,
  "diagnostics": [
    {
      "severity": "warning",
      "code": "pdf.reader-repaired",
      "message": "The document contained recoverable structural errors.",
      "context": { "path": "example.pdf" }
    }
  ],
  "outputs": [
    {
      "kind": "file",
      "role": "primary",
      "path": "input-redacted.pdf",
      "state": "written"
    }
  ],
  "data": {
    "report": {
      "schema_version": 3,
      "pass": false,
      "profile": "Loupe Default",
      "errors": [],
      "warnings": [],
      "fixups_available": []
    }
  }
}
```

| Field | Type | Meaning |
|---|---|---|
| `schema_version` | `integer` | PdfTool result-envelope schema version (`1`). Independent of nested `data.report.schema_version`. |
| `command` | `string` | Command that produced the result (e.g. `preflight`, `info`, `diff`). For a bare invocation this is `help`; for an unknown command it is the offending command string. |
| `version` | `string` | PdfTool application version (`QCoreApplication::applicationVersion()`). |
| `status` | `string` | Stable machine name for `exit_code`. See exit-code taxonomy. |
| `exit_code` | `integer` | Process exit code. `status` and the process exit code always agree. |
| `diagnostics` | `array` | Structured messages recorded during the run (warnings, handled errors). |
| `outputs` | `array` | Files that the command produced (or planned, under `--dry-run`). |
| `data` | `object` | Command-specific payload. Empty object when the command has none. |

## Exit-code taxonomy

| Exit | Enum | `status` | Meaning |
|---:|---|---|---|
| `0` | `Success` | `success` | Command completed and its domain result is clean |
| `1` | `Findings` | `findings` | Command completed correctly but found differences, validation failures, failed checks, etc. |
| `2` | `InvalidInvocation` | `invalid-invocation` | Bad/missing arguments or invalid command |
| `3` | `InputError` | `input-error` | Input missing, unreadable, bad password, corrupt beyond recovery |
| `4` | `ProcessingFailure` | `processing-failure` | Command started but operation could not complete |
| `5` | `PartialOutput` | `partial-output` | Some requested work/output succeeded, some failed |
| `6` | `Cancelled` | `cancelled` | User/signal cancellation |
| `7` | `InternalError` | `internal-error` | Unexpected invariant/internal failure |

Exit `1` does **not** mean the executable malfunctioned. In CI:

```bash
PdfTool diff old.pdf new.pdf --console-format json
```

- exit `1` (`findings`) means "valid comparison, differences found";
- exit `4` (`processing-failure`) means "the comparison could not be performed".

## Diagnostics

```json
{
  "severity": "warning",
  "code": "pdf.reader-repaired",
  "message": "The document contained recoverable structural errors.",
  "context": { "path": "example.pdf" }
}
```

- `severity`: `info` | `warning` | `error`.
- `code`: stable kebab-case identifier, e.g. `cli.invalid-arguments`,
  `cli.unknown-command`, `pdf.document-unreadable`, `pdf.invalid-password`,
  `pdf.reader-warning`, `output.already-exists`, `output.write-failed`,
  `operation.cancelled`. Machine consumers branch on these codes; treat them as
  the stability contract. `message` is human-oriented and may change.
- `context`: optional free-form object (e.g. the offending path).

In JSON mode, handled errors and warnings are captured in `diagnostics` and are
**not** additionally written to stderr. In text/XML/HTML mode the existing
human-facing stderr behavior is preserved.

## Output records

```json
{
  "kind": "file",
  "role": "primary",
  "path": "output.pdf",
  "state": "written"
}
```

`state` values: `written` (file committed), `planned` (`--dry-run`), `partial`
(some requested files written, others failed). Multi-file operations (render,
separate, image extraction, attachment extraction) emit one record per produced
file. A run where some files succeed and some fail reports `partial-output`.

## JSON mode detection

`--console-format json` and `--console-format=json` are detected from the raw
command line before parsing, so that malformed command lines still return a
valid JSON error envelope when JSON was requested. The `preflight` and `ocr`
commands default to JSON because their domain reports are JSON-only; malformed
invocations of those commands therefore also return the envelope. Supplying a
different console format to either command is an invalid invocation.

## Unknown command

An unknown command is an `invalid-invocation` (`exit_code` 2) with a
`cli.unknown-command` diagnostic. Human mode may still print help text, but the
process must no longer appear successful.

## Command data payloads

| Command family | JSON `data` | Typical non-zero clean execution |
|---|---|---|
| `help`, `info`, `info-*`, `statistics`, `fetch-text`, `ink-coverage` | Existing formatter tree | None |
| `diff` | Difference report | `1 findings` |
| `verify-signatures`, `verify-redaction` | Verification report | `1 findings` |
| `preflight` | `{ "report": <existing preflight report> }` | `1 findings` |
| `ocr` | `{ "report": <existing OCR report> }` | `5 partial-output` |
| `render`, `separate`, image/attachment extraction | Summary + `outputs[]` | `5 partial-output` |
| `optimize`, `redact`, encrypt/decrypt, unite, add-bleed | Operation data + final output | Usually `4 processing-failure` on failure |

## Schema

- Machine-readable envelope schema: `schemas/pdftool-result.schema.json`
  (versioned alongside this document).
- Preflight continues to validate against `loupe-preflight/schemas/report.schema.json`
  (unchanged); the envelope carries it under `data.report`.
- OCR carries its report under `data.report` (unchanged domain schema).
