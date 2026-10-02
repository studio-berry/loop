# Evaluate optional-content membership for hidden content (#115)

Category: fixed
Audience: prepress operators and preflight maintainers relying on hidden-layers
Breaking-Change: no
Summary: hidden-layers now evaluates optional-content membership dictionaries (every /P policy and /VE expression) on marked content, Form XObject /OC entries and annotation /OC entries under both the View and Print usages with the default configuration's /AS events, and reports content that is hidden in print or differs between screen and print; a governor it cannot evaluate is reported incomplete instead of clean. Adds the ocmd-print-divergence regression fixture and lands the hidden-layers-ocmd backlog row.
