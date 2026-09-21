Category: added
Audience: operators, integrators
Breaking-Change: no
Summary: Add `PdfTool preflight --report-file <file>`, which writes the preflight report in its retained form - the redacted payload the operation-history chain stores and a certificate hashes - so one run's report can be archived, handed on, or bound to `report_digest` without re-running Loop. `repair` already had `--report-file`; preflight now matches it, which also gives the portable evidence bundle (issue #589) a CLI producer for the report input it requires.
