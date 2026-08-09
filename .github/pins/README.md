# Supply-chain pins

Every external build-time dependency the CI consumes is pinned to an immutable
identifier and, where the artifact is a blob, a SHA-256 digest. The policy is
enforced on every CI run by `scripts/ci/check_supply_chain_pins.py`:

- **Workflow actions**: every `uses:` reference must be a full 40-character
  commit SHA (no `@vN`, `main`, `latest`, `continuous`).
- **vcpkg**: the `default-registry.baseline` in `vcpkg-configuration.json` is
  the single source of truth, and workflows derive their checkout from it.
- **Packaging tools**: `packaging-tools.json` below carries the identity and
  digest of every binary downloaded outside the repository.

## Source of truth

`.github/pins/packaging-tools.json` is the canonical record for every external
packaging binary. Workflows read from it at runtime and never hard-code digests
inline. Its layout:

| Key | Used by | Pins |
|-----|---------|------|
| `appimagetool` | `LinuxInstall.yml` | upstream repo, commit, release-asset ID, SHA-256 |
| `appimageRuntime` | `LinuxInstall.yml` | upstream repo, commit, release-asset ID, SHA-256 |
| `linuxdeployqt` | `LinuxInstall.yml` | upstream repo, commit, release-asset ID, SHA-256 |
| `wix` | `WindowsInstall.yml` | versioned release URL + SHA-256 |
| `windowsSdk` | `WindowsInstall.yml` | MakeAppx.exe SHA-256 per SDK version |
| `digicertKeylocker` | `WindowsInstall.yml` | KeyLocker toolset URL + SHA-256 (signing only) |
| `flatpak` | `LinuxFlatpak.yml` | runner label + apt package versions |
| `deb` | `ci.yml` | runner label + dpkg version |

## Refreshing a pin

1. Let Dependabot propose GitHub Action SHA updates; review the release tag,
   commit, and action diff before accepting one.
2. For packaging binaries, resolve the exact upstream commit/release asset and
   download the candidate outside the production workflow.
3. Verify any upstream signature or published digest, then compute an
   independent SHA-256.
4. Update `packaging-tools.json` so the diff shows the old/new identity and
   old/new digest together.
5. Run `python3 scripts/ci/check_supply_chain_pins.py`, the affected packaging
   workflow, and the existing AppImage/MSI/MSIX smoke tests.
6. Record the reviewed version and evidence in the commit or pull request
   description before merging.

Never replace an immutable pin with a moving tag such as `main`, `latest`, or
`continuous`.

## Unpinned signing toolchain

`digicertKeylocker.version`, `.url`, and `.sha256` are `null` by default. The
MSI signing step guards against this: if `SIGN_MSI` is enabled and the pin block
is empty, the build fails with a hard error rather than silently downloading the
KeyLocker toolset from a mutable release URL.

[CI workflow documentation](../CI.md)
