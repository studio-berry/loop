Category: changed
Audience: developers and operators
Breaking-Change: yes
Summary: The save mode an operation declares is now a floor at the Core transaction and write
boundary. `PDFRepairTransaction::setRequestedSavePolicy()` accepts a stricter policy but refuses a
weaker one before analysis or mutation, and `pdf::validateSaveRequest()` refuses any candidate write
whose output resolves to the trusted input file. `redact`, `add-bleed` and `rgb-to-cmyk` therefore
exit 4 with `save-policy.refused` when the output path is the input path — those in-place invocations
previously succeeded with `--overwrite`, so write the candidate to a new path. Redaction also
declares a full rewrite, so it can never produce an incremental update.
