# Close the eight preflight corpus fixture gaps (#668)

Category: fixed
Audience: developers and prepress maintainers reading the generated preflight corpus-coverage map
Breaking-Change: no
Summary: Add golden-corpus fixtures, test profiles, and committed snapshots that produce findings for dieline, processing-steps, font-integrity, hidden-layers, invisible-content, obscured-content, off-page-content, and thin-parts (plus conformance-claims), remove their corpus_gap overlay reasons, and leave corpus_gaps empty so the corpus gate can prove those checks fire.
