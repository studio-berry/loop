# Loupe versioning

Loupe releases follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).
The machine-readable policy is [`version-policy.json`](version-policy.json).
`scripts/ci/check_version_policy.py` runs in CI so a four-part CMake version or
a release-draft parser that still expects one fails before a tag is cut.

The declarations below are intentionally machine-readable by that check:

- Scheme: SemVer 2.0
- Canonical version: `PDF4QT_VERSION` in root `CMakeLists.txt`
- Format: MAJOR.MINOR.PATCH
- Current version: 0.1.0
- Pre-release: alpha
- Git tags: `vMAJOR.MINOR.PATCH` with optional pre-release / build metadata
- Windows Appx version: MAJOR.MINOR.PATCH.0 (`PDF4QT_WINDOWS_VERSION`)
- Release workflow: `.github/workflows/CreateReleaseDraft.yml`

The current line is **0.1.0-alpha**. Display strings, PdfTool's envelope
`version`, and git tags use that full pre-release identifier. CMake's
`project(VERSION)` stays `0.1.0` because it only accepts numeric components.

## Milestone remap

Earlier 0.0.x planning numbers map onto the 0.1 line as follows:

| Former | Current |
|--------|---------|
| 0.1.0-alpha (this line) | current pre-release |
| 0.0.3 | 0.1.1 |
| 0.0.4 | 0.1.2 |
| 0.0.5 | 0.1.3 |
| 0.0.6 | 0.1.4 |

Do not keep using 0.0.3–0.0.6 as living gates. Historical `0.0.1` / `0.0.2`
tags and issue prefixes stay as completed history.

## Public API

For version bumps, the public API is:

- PdfTool CLI flags, commands, and the JSON result envelope (`version` in
  [PDFTOOL_CLI_CONTRACT.md](PDFTOOL_CLI_CONTRACT.md))
- Shared library ABI (`VERSION` / `SOVERSION` on `Pdf4QtLibCore` and plugins)
- Documented Editor / PageMaster behavior that callers and plugins rely on

JSON `schema_version` fields (preflight profiles, reports, PdfTool envelope)
are independent contracts. Bump those only with a coordinated engine change;
do not fold a schema bump into a product patch unless the schema itself is
the fix.

While the major version is `0`, SemVer allows breaking changes on minor
bumps. Prefer documenting them; do not treat `0.y.z` as a compatibility
promise.

## Increment rules

Given `MAJOR.MINOR.PATCH`:

| Change | Bump |
|--------|------|
| Breaking CLI, ABI, or documented behavior | MAJOR (reset minor and patch to 0) |
| Backward-compatible feature | MINOR (reset patch to 0) |
| Backward-compatible bug fix | PATCH |

Set `PDF4QT_VERSION_PRERELEASE` in root `CMakeLists.txt` for `-alpha`,
`-rc.1`, and similar labels. Clear it for a final `0.1.0`. Build metadata
(`+githash`) belongs on tags only.

## Source of truth and packaging

1. Set `PDF4QT_VERSION` to the three-part SemVer core and optional
   `PDF4QT_VERSION_PRERELEASE` in root `CMakeLists.txt`.
2. Git tags are `v${PDF4QT_VERSION}` or `v${PDF4QT_VERSION}-${prerelease}`.
A repository tag ruleset requires that pattern (optional `v` + SemVer 2.0,
including pre-release). Non-SemVer tags are rejected.
3. `CreateReleaseDraft.yml` reads those CMake values and creates `v${version}`.
4. MSI / WiX uses the three-part core. Appx `Identity.Version` requires four
   components, so CMake derives `PDF4QT_WINDOWS_VERSION` as
   `${MAJOR}.${MINOR}.${PATCH}.0`.

Do not revive the inherited four-part `x.y.z.w` product version. Upstream
PDF4QT may still use that scheme; Loupe tags and `PDF4QT_VERSION` do not.
`RELEASES.txt` entries such as `V: 1.6.0.0` are upstream history, not Loupe's
current version.
