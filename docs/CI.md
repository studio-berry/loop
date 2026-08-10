# CI and diagnostic artifacts

The inherited build workflow currently targets `master` for the **Linux** and
**Windows** build-and-test jobs. That trigger drift is explicitly tracked in
[GitHub issue #232](https://github.com/studio-berry/loupe/issues/232); it is not
the active Loupe branch policy.
These are the two platforms Loupe V1 supports; **macOS** CI is a **post-V1**
track under [MIC-336](https://linear.app/mbx2/issue/MIC-336) /
[docs/PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md). Packaging artifacts are produced
only for `master` pushes and manual workflow runs.

The standalone `Documentation truth` workflow runs for the policy branches
`dev` and `stable`
pull requests and pushes. It checks every ADR's verification header and fails
when [`docs/generated/architecture-catalog.json`](generated/architecture-catalog.json)
is stale. The branch names above reflect the current branch policy; issue #232
tracks the separate workflow-trigger migration from the inherited `master`
configuration.

Note that the `pull_request` trigger sets `paths-ignore` for `docs/**` and
`**/*.md`, so documentation-only pull requests do not run `build_ubuntu` or
`build_windows`. The `ci_ok` job always runs and is the status check to require
in branch protection — requiring the build jobs directly would leave docs-only
pull requests permanently pending.

When a Windows test run fails, GitHub Actions uploads its CTest logs as the
`windows-test-logs` artifact. Store any intentional, long-lived diagnostic
artifact in the related issue or release attachment; do not add local build or
test output to the repository.

## Updating pinned workflow dependencies

Workflow actions, vcpkg, and binary packaging tools are pinned to exact
revisions or SHA-256 checksums. The source of truth for packaging tools is
`.github/pins/packaging-tools.json` (see its README); the vcpkg baseline is the
`default-registry.baseline` in `vcpkg-configuration.json`. Workflows derive
their vcpkg checkout and all tool digests from those files at runtime, and the
`Verify supply-chain pins` step (`scripts/ci/check_supply_chain_pins.py`)
fails the build if any action is not a full commit SHA, any package URL is
mutable (`continuous` / `latest`), or a pin is missing.

To refresh a pin:

1. Review the upstream release notes and commit diff.
2. Update `.github/pins/packaging-tools.json` (for packaging tools) or the
   workflow's copy of a full action SHA.
3. Run the affected workflow manually and inspect its logs and generated
   artifact; the verify step enforces the digest.
4. Record the reviewed version in the commit or pull request description.

Do not replace a pin with a moving tag such as `main`, `latest`, or `continuous`.
DigiCert KeyLocker (`MSI` signing) must additionally be added to
`digicertKeylocker` in `packaging-tools.json` before `SIGN_MSI` can proceed;
the signing step refuses to run against an unpinned toolchain.
