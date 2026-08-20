# GitHub milestones

Canonical milestone text for [studio-berry/loupe](https://github.com/studio-berry/loupe) is maintained here and aligned with the Notion Loupe [Roadmap](https://app.notion.com/p/38f9cb079ddb804a96dbe26b8d86e84f).

## Sequence

| Milestone | Status | Former alias |
|-----------|--------|--------------|
| 0.0.1 | Historical | — |
| 0.0.2 | Historical (recovery baseline merged to `stable`) | — |
| 0.1.0 | Shipped as `0.1.0-alpha` | — |
| 0.1.1 | Living | 0.0.3 |
| 0.1.2 | Living | 0.0.4 |
| 0.1.3 | Living | 0.0.5 |
| 0.1.4 | Living | 0.0.6 |

See also [`docs/VERSIONING.md`](../VERSIONING.md) for the SemVer remap.

## Sync

Apply descriptions to GitHub with a token that has `issues: write` on the repository:

```bash
python scripts/github/sync_milestones.py --apply
```

Dry run (default):

```bash
python scripts/github/sync_milestones.py
```

The script matches milestones by title and updates description plus optional open/closed state from [`manifest.json`](manifest.json).
