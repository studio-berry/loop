# ADR-008: Rewrite generated artifacts out of unreleased 0.0.2 history

**Status:** implemented
**Implemented-at:** b47d63e47bebfc1fdd74a75374fc9cecccddc325
**Last-verified:** 2026-08-12 @ b47d63e47bebfc1fdd74a75374fc9cecccddc325
**Superseded-by:** none
**Date:** 2026-08-12
**Deciders:** GitHub #265 operator exception (rewrite both `dev` and `stable`)

## Context

[#265](https://github.com/studio-berry/loop/issues/265) asked to decide
whether to rewrite or retain generated dependency and build blobs already
present in the 195 unreleased `dev` commits, and originally recommended a
`dev`-only rewrite because `stable` had not yet received that history.

That window closed when [PR #188](https://github.com/studio-berry/loop/pull/188)
merged to `stable` on 2026-08-13. After the merge, both `origin/dev` and
`origin/stable` still contained the same 982 blobs (~400.5 MiB):
`.docker-vcpkg*`, `build-fuzz-docker/` (including a 45 MiB
`libLoopLibCore.so`), `debug-b0e75b.log`, `scripts/debug-pr188.*`, and stray
`loop-ocr` bytecode. Branch tips were already clean ([#249](https://github.com/studio-berry/loop/pull/249),
[#258](https://github.com/studio-berry/loop/pull/258)); only history held the
blobs.

Rewriting only `dev` would not reclaim GitHub storage. Rewriting `stable`
contradicts the original #265 safety constraint and required an explicit
operator exception.

## Decision

Rewrite **both** protected branches with `git filter-repo --invert-paths` in
an isolated clone, then force-update only `refs/heads/dev` and
`refs/heads/stable`. Do not rewrite `dev-gui` or tag `v0.0.1-alpha` (neither
contained matching blobs). Do not create a remote backup tag.

Approved path allowlist (history only; tips were already clean):

- `.docker-vcpkg`
- `.docker-vcpkg-cache/`
- `.docker-vcpkg-installed/`
- `build-fuzz-docker/`
- `debug-b0e75b.log`
- `scripts/debug-pr188.sh`
- `scripts/debug-pr188.ps1`
- `loop-ocr/tools/__pycache__/`

Keep `vcpkg.json`, `vcpkg-configuration.json`, overlay ports, test fixtures,
and `docs/generated/architecture-catalog.json`.

## Outcome

- `dev`: `b0d682ba8fa1645e2e122b1dee125c4ffdddbaf9` -> `b47d63e47bebfc1fdd74a75374fc9cecccddc325` (tree `a519900f9b7b88d531927ba71a23d098b011b978` unchanged)
- `stable`: `41d85ffe1ea91626e9e84fc28d7eebbef6cc79d4` -> `f42538d3fe1ae41bc2dc525f90c332b387bcf73d` (tree `daec4589efcf8a38e058c98bca44487c1929699a` unchanged)
- `dev-gui`: `3b99b677786a6fffef081cab72210a25b2f6c06b` unchanged
- `v0.0.1-alpha`: `ea6193281758c23350f018843f919253ffd8022b` unchanged

`git-filter-repo` First Changed Commit:
`13666aaac24d7578a093b3fc53f4d09769766b50` ->
`20d90ad3b4c658b8c7843ad39117fb0bcf7bf78f`.

A fresh clone of the rewritten tips has zero matching objects. GitHub
pull-request refs still advertise the old blobs until Support GC; 91 listed
PRs have heads in the rewritten commit map. Local clones and topic branches
must not push unrebased history or the blobs return.

## Consequences

- Every commit SHA after the first changed commit is new. Issue and PR
  citations that pointed into that range are stale.
- Signatures on rewritten commits do not verify. Branch protection does not
  require signed commits.
- GitHub Support must dereference PR refs and run GC before clone size on
  GitHub's side fully drops. Support may decline if they treat this as
  non-sensitive bloat rather than leaked secrets.
- Open PR #282 was based on pre-rewrite `stable` and still contained generated
  objects; it must not be merged. Recreate it from rewritten `stable` if the
  catalog fix is still needed there.
- Future growth remains blocked by `.gitignore` plus
  `scripts/ci/check_generated_dependency_paths.py` and
  `scripts/ci/check_source_integrity.py`.
