<!-- GENERATED FILE: edit agent-policy.json and run scripts/agent/generate-adapters.py --write -->
# Loop agent policy adapter

Repository: `studio-berry/loop`; version: `0.2.0-alpha`; language: `C++20`; minimum Qt: `6.11.1`.

## Branches and safety

- Integration: `dev`; qualification: `unstable`; release/default: `stable`; topic branches start from `dev`. Promotion: `dev` → `unstable` → `stable`.
- Protected branches: `unstable`, `stable`. Do not commit, push, merge, force-push, or rewrite history without approval.
- Keep private data, credentials, logs, scratch plans, and build artifacts outside the repository. Do not edit vendored dependencies unless explicitly scoped.

## Autonomous verification budget

Allowed without additional approval:
- format touched files
- static analyze touched translation units
- build affected targets in existing build
- run mapped focused tests
- run source and contract checks

Approval is required for:
- dependency or toolchain changes
- configure or reconfigure
- clean or full rebuild
- packaging or installation
- external writes
- push or merge
- history rewrite
- upstream sync
- signing or credential operations

## Required proof and changelog

- After implementation, run `python scripts/agent/check-change.py --base origin/dev` (or the equivalent base SHA). Treat an incomplete result as not proven.
- Every PR adds exactly one `changes/<sanitized-head-branch>.md` fragment. Required fields: Category, Audience, Breaking-Change, Summary. Categories: added, changed, fixed, security, internal.
- Use `internal` for tooling or documentation changes; it still requires a fragment.
- Do not invent a public contract when a protected interface, schema, persistence format, central type, or root build contract must change; stop and report the contract change.

## Module placement

- Core PDF logic belongs in `LoopLibCore`; it must not depend on Widgets.
- Interactive plugins belong in `LoopEditorPlugins` hosted by the Editor; batch geometry belongs in PageMaster; unattended pipelines belong in PdfTool.
- Consult the generated architecture catalog and current code/tests for dynamic facts; narrative docs are not authoritative when they conflict.
- Record parser/writer/renderer divergences from the upstream engine in `docs/UPSTREAM_DIVERGENCE.md`. Cosmetic Loop-only code does not belong there.

Generated adapter: `.cursor/agent-policy.md`.
