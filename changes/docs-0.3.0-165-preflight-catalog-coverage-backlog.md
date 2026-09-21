# Enrich the preflight check catalog and generate the coverage backlog

Category: internal
Audience: developers and prepress maintainers reading the generated preflight catalog
Breaking-Change: no
Summary: Every preflight check row now carries parameters with ranges, its severity model, its finding evidence fields, and the fixups that apply, and the generator emits docs/generated/preflight-coverage-backlog.json as a prioritised gap register cross-referenced to verified GitHub issues and landed checks.
