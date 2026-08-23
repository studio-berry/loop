# Architecture source of truth

Architecture documentation is a navigational aid, not a substitute for the
implementation. When sources disagree, resolve the conflict in this order:

1. Tests and current code on `dev`.
2. An explicitly re-verified issue status block.
3. An accepted ADR whose `Implemented-at` commit is still current.
4. Notion product and strategy vision.
5. `agent-policy.json` and generated adapters (`AGENTS.md`, Claude/Cursor files).
6. README and narrative repository maps.
7. Migrated Frisket historical text.

ADRs under [`docs/adr/`](adr/) carry machine-checked `Status`,
`Implemented-at`, `Last-verified`, and `Superseded-by` metadata. The
verification header is checked by
[`scripts/generate-architecture-catalogs.py`](../scripts/generate-architecture-catalogs.py).

The generated
[`architecture-catalog.json`](generated/architecture-catalog.json) is the
current factual inventory of policy branches, workflow trigger branches,
built-in preflight checks, registered repair operations, runtime/schema
versions, numbered architecture invariants, and CMake test targets.
Regenerate it after changing one of its source files:

```bash
python3 scripts/generate-architecture-catalogs.py --write
python3 scripts/generate-architecture-catalogs.py --check
```

The branch policy itself is intentionally a small reviewed input at
[`docs/branch-policy.json`](branch-policy.json); the generator emits its branch
names into the catalog and the documentation workflow checks the committed
result. Product versioning is the matching input at
[`docs/version-policy.json`](version-policy.json) (Semantic Versioning 2.0,
current `0.2.0-alpha`). Ephemeral topic branches are not architecture facts
and are not copied into the catalog.
