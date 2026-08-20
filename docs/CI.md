# CI and diagnostic artifacts

Pull requests to `dev` run the Linux `agent-fast / build` workflow as the
required integration gate. It classifies the diff, runs source and contract
checks, compiles affected targets, and runs focused tests. Pull requests
require one structured changelog fragment under `changes/` named after the
head branch. Subsequent `dev` pushes skip that PR-only check so a merged
topic fragment is not rejected for not being `changes/dev.md`. Stacked topic
branches may carry their parent fragments, but every added fragment is
validated. Format and clang-tidy run on added, modified, renamed, or copied
C/C++ files only; deleted paths still classify modules. These are the fast
checks for
the shared integration baseline. The full Linux and Windows build-and-test
jobs run for release qualification. These are the two platforms Loupe V1
supports; **macOS** CI is a **post-V1** track under
[MIC-336](https://linear.app/mbx2/issue/MIC-336) /
[docs/PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md). Packaging artifacts are
produced only for `stable` pushes and manual workflow runs.

The standalone `Documentation truth` workflow runs for the policy branches
`dev` and `stable` pull requests and pushes. It checks every ADR's verification
header and fails when
[`docs/generated/architecture-catalog.json`](generated/architecture-catalog.json)
is stale. Product versioning is SemVer 2.0 (`0.1.0-alpha`); CI also runs
`scripts/ci/check_version_policy.py`.

The `release_ok` job in `.github/workflows/release-gate.yml` is the single
GitHub Actions status required by the protected `stable` branch. It always
runs and fails when any required dependency failed, was cancelled, was
skipped, or did not report. Requiring the platform-specific jobs directly
would create multiple checks for the same gate. The release-gate workflow
runs for every pull request targeting `stable` and for `merge_group` events;
it has no path filters. `dev` requires `agent-fast / build` for merging;
`stable` requires `release_ok`.

Hosted fuzzing (`.github/workflows/fuzz.yml`) validates the manifested
regression corpus under `Fuzz/corpus/` and runs each harness's owned seeds
with `-runs=0` before time-bounded mutation.

When a Windows test run fails, GitHub Actions uploads its CTest logs as the
`windows-test-logs` artifact. Store any intentional, long-lived diagnostic
artifact in the related issue or release attachment; do not add local build or
test output to the repository.

## Generated dependency state

The dependency source of truth remains `vcpkg.json`,
`vcpkg-configuration.json`, and the reviewed overlay ports. Install trees,
downloaded packages, and binary caches are generated state and must not be
committed. CI stores its vcpkg downloads and install output in the GitHub
Actions cache; local Docker/fuzz workflows may use ignored `.docker-vcpkg*`
directories.

`scripts/ci/check_generated_dependency_paths.py` inspects the Git index and
fails CI if any known generated dependency path is tracked again.

### Windows local test executables

Windows test executables need the configured Qt and Loupe/vcpkg runtime DLLs beside
the executable. If the dependency set is not deployed into the build output,
the process can wait behind a missing-DLL system error instead of printing a
normal test failure. For a Release test directory, run `windeployqt` with the
configured Qt root and deploy the Qt/Loupe dependencies into that same
directory before invoking CTest. Generated DLLs and plugin directories remain
local build output and must not be committed.

## Tracked source integrity

`scripts/ci/check_source_integrity.py` runs before Qt/vcpkg configure on every
`ci.yml`, `release-gate.yml`, and `fuzz.yml` invocation. It inspects the Git
index (`git ls-files`) and fails when tracked content includes:

- build trees (`build/`, `build-*`), `.docker-vcpkg`, or `CMakeCache.txt`
- root `debug-*.log` files or one-off `scripts/debug-*` scripts
- unresolved merge-conflict markers
- tracked files over 5 MB without an explicit allowlist entry
- whitespace problems reported by `git diff --check` over the full tree
- fuzz regression seeds or preflight fixture PDFs that are not listed in their
  corpus manifests

Negative fixtures live in `scripts/ci/test_check_source_integrity.py`.

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

## Sentry debug files

Windows Release builds with `LOUPE_ENABLE_SENTRY` emit PDBs (`/Zi` +
`/DEBUG:FULL`) so crashpad minidumps can be symbolicated. After the Windows
CI and MSI packaging jobs, `scripts/ci/upload_sentry_debug_files.ps1`
uploads Loupe PDBs to `berry-studios/loupe-pdf` on the EU region
(`https://de.sentry.io`) using the pinned `sentryCli` binary. GitHub
Actions cannot reference `secrets` in `if:` conditionals, so the workflow
always runs the step; `upload_sentry_debug_files.ps1` no-ops when
`SENTRY_AUTH_TOKEN` is unset (fork pull requests). PDBs are not installed
into the MSI; they stay on the Sentry debug-file store. Store the token as
the repository secret `SENTRY_AUTH_TOKEN` (`project:releases` or broader);
do not commit it.
