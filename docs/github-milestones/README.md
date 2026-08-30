# GitHub milestones

Canonical milestone text for [studio-berry/loupe](https://github.com/studio-berry/loupe) is maintained here and aligned with the Notion Loupe [Roadmap](https://app.notion.com/p/38f9cb079ddb804a96dbe26b8d86e84f).

## Sequence

| Milestone | Status | Former alias |
|-----------|--------|--------------|
| 0.0.1 | Historical | — |
| 0.0.2 | Historical (recovery baseline merged to `stable`) | — |
| 0.1.0 | Shipped as `0.1.0-alpha` | — |
| 0.1.1 | Living | 0.0.3 |
| 0.2.0 | Living | 0.0.4 (supersedes retired `0.1.2` title) |
| 0.3.0 | Living | 0.0.5 (supersedes retired `0.1.3` title) |
| 0.4.0 | Living | 0.0.6 (supersedes retired `0.1.4` title) |
| 0.5.0 | Planned (proposed) | — |
| 0.6.0 | Planned (proposed) | — |
| 0.7.0 | Planned (proposed) | — |
| 0.8.0 | Planned (proposed) | — |
| 0.9.0 | Planned (proposed) | — |
| 0.10.0 | Planned (proposed) | — |

The living release train is **0.1.1 → 0.2.0 → 0.3.0 → 0.4.0**. Retired `0.1.2`–`0.1.4` GitHub milestone titles are closed by the sync script.

The planned continuation **0.5.0 → 0.6.0 → 0.7.0 → 0.8.0 → 0.9.0 → 0.10.0** is scoped in
[`docs/ROADMAP_0.5.0-0.10.0.md`](../ROADMAP_0.5.0-0.10.0.md) and remains proposed until the
canonical Notion roadmap is amended; each planned milestone activates only on its
predecessor's release acceptance.

## Sync

Apply descriptions to GitHub with a token that has `issues: write` on the repository:

```bash
python scripts/github/sync_milestones.py --apply
```

Dry run (default):

```bash
python scripts/github/sync_milestones.py
```

The script matches milestones by title, creates missing canonical milestones, updates description plus optional open/closed state from [`manifest.json`](manifest.json), and closes retired titles listed under `retire`.
