# Repo map

Loupe repository layout, fork relationship, branch policy, and upstream
tracking policy.

## Repositories

| Role | Repository | Branch |
|------|------------|--------|
| Loupe canonical repository | [studio-berry/loupe](https://github.com/studio-berry/loupe) | `stable` (default/release), `dev` (integration) |
| Upstream PDF engine source | [JakubMelka/PDF4QT](https://github.com/JakubMelka/PDF4QT) | `master` (upstream only) |

Loupe owns the product decisions, branding, release policy, and downstream
changes. PDF4QT remains the upstream source for the PDF engine and inherited
tooling. Do not infer Loupe branch policy from upstream's `master` branch.

## Branch policy

- `dev` is the integration branch.
- `stable` is the release branch and repository default.
- Topic branches start from `dev`, stay focused, and merge back to `dev`.
- Releases promote a verified `dev` state to `stable`.
- `master` is not an active Loupe branch.

The reviewed machine-readable policy is
[`branch-policy.json`](branch-policy.json). The current factual branch and
workflow audit is tracked in GitHub issue [#232](https://github.com/studio-berry/loupe/issues/232).

## Versioning

Loupe uses [Semantic Versioning 2.0](https://semver.org/). The current product
version is **0.1.0-alpha**. Policy: [`version-policy.json`](version-policy.json)
and [`VERSIONING.md`](VERSIONING.md). Former 0.0.3–0.0.6 gates are 0.1.1–0.1.4.

## Upstream tracking policy

Parser, writer, and renderer divergences from upstream PDF4QT are
recorded in [`UPSTREAM_DIVERGENCE.md`](UPSTREAM_DIVERGENCE.md). Cosmetic
Loupe-only code does not belong there. Re-run the mapped tests after an
authorized sync; a clean merge is not verification.

### Policy: on-demand GitHub Sync fork

Pull upstream changes only when explicitly requested, through GitHub's **Sync
fork** UI or an intentional local `gh repo sync`. Do not automatically merge
upstream in CI.

When syncing, preserve Loupe product and licensing decisions. Take upstream
code fixes in shared engine code unless a current Loupe ADR or issue says
otherwise. Do not push Loupe branding, product UX, or release policy upstream.

### Conflict handling

Likely conflict files include `README.md`, `LICENSE`, `.github/**`,
`CMakeLists.txt`, and `RELEASES.txt`.

- Keep Loupe branding and licensing in `README.md` / `LICENSE`.
- Prefer upstream code fixes everywhere else, subject to current Loupe tests
  and accepted ADRs.
- Run targeted builds/tests after an authorized sync; do not treat a clean
  merge as verification.
- Record the sync in `RELEASES.txt` or a short merge commit message with the
  upstream SHA.

## Source-of-truth and generated facts

Resolve documentation conflicts using
[`architecture-source-of-truth.md`](architecture-source-of-truth.md). The
generated [`architecture-catalog.json`](generated/architecture-catalog.json)
emits branch policy names, workflow trigger branches, the Core preflight check
catalog, registered repair operations, schema versions, schema kinds, coverage
holes, numbered architecture invariants, and CMake test targets. CI runs
`scripts/generate-architecture-catalogs.py --check` so stale narrative or
catalog claims fail before merge.

Intentional fork-only behavior is listed in
[`UPSTREAM_DIVERGENCE.md`](UPSTREAM_DIVERGENCE.md). Update that register in the
same change that introduces a new divergence, and run the preflight corpus plus
targeted Core tests before merging an authorized sync.

## Layout

| Area | Path | Purpose |
|------|------|---------|
| Core PDF library | `Pdf4QtLibCore/` | Shared PDF parsing, rendering, preflight, and repair logic |
| Interactive editor | `Pdf4QtEditor/`, `Pdf4QtLibGui/` | Primary interactive shell and plugin host |
| Headless CLI | `PdfTool/` | Automation, batch checks, rendering, and repair |
| Page production | `Pdf4QtPageMaster/` | Batch geometry, assembly, and production export |
| Editor plugins | `Pdf4QtEditorPlugins/` | Editor-only capabilities |
| Tests | `UnitTests/` | Qt Test targets declared in `UnitTests/CMakeLists.txt` |
| Preflight contract | `loupe-preflight/` | Profiles, schemas, examples, and report documentation |
| Architecture records | `docs/adr/`, `docs/` | Decisions, policy, plans, and generated factual catalogs |

For module placement and build constraints, see [`AGENTS.md`](../AGENTS.md).
