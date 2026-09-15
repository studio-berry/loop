Category: added
Audience: developers
Breaking-Change: no
Summary: Seed the empty fuzz_pdf_parser, fuzz_content_stream, and fuzz_stream_filters corpora with small synthetic inputs, add malformed-PDF preflight goldens (truncated xref, cyclic Kids, wrong generation, hostile object stream, encrypted-without-password) so parser hangs and crashes are covered without mutation luck, require every harness to own at least one manifested seed, and mark Fuzz/corpus/**/*.bin as binary so ASCII seeds keep stable checksums. Encrypted-without-password is a preflight golden only; it is not a fuzz seed because the parser harness password callback always succeeds.
