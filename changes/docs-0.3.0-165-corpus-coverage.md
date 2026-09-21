# Publish the preflight corpus-coverage map and the reasons a gap exists

Category: internal
Audience: developers and prepress maintainers reading the generated preflight catalog
Breaking-Change: no
Summary: The generator emits docs/generated/preflight-corpus-coverage.json, recording for every registered preflight check which golden-corpus fixtures report one of its findings, which were inspected clean and which never completed an inspection, plus the rows no fixture exercises at all; a covered row without an exercising fixture fails closed, an unexercised row must carry a written reason, a finding that reports an inspection failure does not count as coverage, and unfiled coverage-backlog rows carry their deferral reason as data instead of only in prose.
