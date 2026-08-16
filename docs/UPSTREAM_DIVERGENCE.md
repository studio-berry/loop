# Upstream divergence register

Loupe is a product fork of [JakubMelka/PDF4QT](https://github.com/JakubMelka/PDF4QT).
This register lists intentional divergences that an upstream **Sync fork** must
not clobber. Update this file in the same change that introduces a new
divergence. Run the preflight corpus and targeted Core tests **before** merging
an authorized sync. See [REPO_MAP.md](REPO_MAP.md).

| Area | Loupe behavior | Do not take from upstream |
|------|----------------|---------------------------|
| Branch policy | `stable` / `dev`; `master` inactive | Upstream `master`-centric docs |
| Versioning | SemVer `PDF4QT_VERSION` | Four-part product versions |
| Preflight | `PreflightEngine` + verdict reducer | Replacing fail-closed incomplete with pass |
| Provenance | One SQLite `PDFOperationHistoryStore` | Parallel audit JSONL ledgers |
| Identity | `PDFArtifactIdentity` vs `PDFRevisionIdentity` | Collapsing those types |
| Plugins | Packaged `PDF4QT_PLUGINS_DIR`; ABI check before instance | Unrestricted user plugin dirs |
| Branding | Loupe README / LICENSE / packaging | PDF4QT product naming |
