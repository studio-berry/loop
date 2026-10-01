# Audit glyph coverage of shown codes in embedded fonts (#114)

Category: fixed
Audience: prepress operators and preflight maintainers relying on font-integrity
Breaking-Change: no
Summary: font-integrity now resolves every character code or CID shown in page content, Form XObjects and annotation appearance streams and reports a finding, naming page, font and missing codes, when one maps to no glyph, .notdef, or an empty outline, so an embedded subset that parses but lacks a used glyph no longer passes clean. Adds the font-glyph-missing regression fixture and lands the font-glyph-coverage backlog row.
