# Publish the preflight corpus-coverage map and the reasons a gap exists

Category: internal
Audience: developers and prepress maintainers reading the generated preflight catalog
Breaking-Change: no
Summary: The generator emits docs/generated/preflight-corpus-coverage.json, recording for every registered preflight check which golden-corpus fixtures produce its findings and which rows no fixture exercises, with a fail-closed rule that a covered row must have a fixture and an unexercised row must carry a written reason; unfiled coverage-backlog rows now carry their deferral reason as data instead of only in prose, and the corpus-coverage guard runs beside the catalog guard in the source_integrity CI job.
