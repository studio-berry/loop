# Schema compatibility closeout (#266)

Category: added
Audience: developers
Breaking-Change: no
Summary: Read current schema versions only from the compatibility matrix, cover every JSON schema kind with current/previous golden round-trips, fail closed when a document identifies no schema kind, report a newer-minor document's own version, add the shared schema compatibility diagnostic on Core and `PdfTool schema`, derive discovered schema versions from the matrix, and record a `SchemaMigrated` provenance event when a history database is upgraded.
