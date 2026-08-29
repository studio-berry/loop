# Platform support (Windows, Linux)

Loupe PDF V1 ships on **two** desktop platforms: Windows and Linux. This
document is the source of truth for supported OS, install layouts, and the
**cross-platform compatibility pass** over modules that are already
feature-complete.

**Linear:** [MIC-336](https://linear.app/mbx2/issue/MIC-336) (R-014) — retargeted
as a **post-V1** epic; see [macOS (post-V1)](#macos-post-v1).
**Related:** [MIC-301](https://linear.app/mbx2/issue/MIC-301) (Windows MSI),
[MIC-328](https://linear.app/mbx2/issue/MIC-328) (Flatpak permissions),
[MIC-329](https://linear.app/mbx2/issue/MIC-329) (licensing checklist),
`docs/PACKAGING_LICENSING.md`

## Supported platforms

| Platform | Status | CI | Official packages |
|----------|--------|----|-------------------|
| **Windows** (x64) | Supported; Session 07 package-boundary gate targets MSI | `ci.yml` + `WindowsInstall.yml` | x64 MSI (`WixInstaller/`) |
| **Linux** (x86_64) | Supported; Session 07 package-boundary gate targets AppImage | `ci.yml` + `LinuxInstall.yml` | x86_64 AppImage (`CreateReleaseDraft.yml`) |
| **macOS** | **Not supported for V1** — source builds best-effort | None | None |

Session 07 qualifies only the Linux x86_64 AppImage and Windows x64 MSI. Flatpak,
MSIX, and portable ZIP remain outside this gate even when their build jobs exist.

Unsupported for V1: macOS, iOS, Android, other BSDs. Community builds elsewhere
are best-effort only and produce no official release artifacts.

> **Claim discipline.** Do not describe macOS as a supported platform in README,
> marketing copy, or release notes while this table says otherwise. The code is
> expected to compile on macOS, but "compiles" is not "supported": there is no CI
> job proving it, no signed package, and no notarization.

## Install and plugin layout

| Platform | Binaries | Plugins (`LOUPE_PLUGINS_DIR`) | Preflight profiles |
|----------|----------|--------------------------------|--------------------|
| Windows | `<prefix>/bin` (beside MSI tree) | `<prefix>/pdfplugins` (relative `../pdfplugins`) | `share/loupe/profiles/` in bundle |
| Linux | `<prefix>/bin` | `<prefix>/lib/loupe` | `/usr/share/loupe/profiles/` (AppImage internal `usr/` layout; the `.deb`'s layout is the same but the package doesn't run — see Supported platforms above) |

Flatpak packaging uses the bounded `--filesystem=home` permission; see
`docs/FLATPAK_SANDBOX.md` for named-path overrides and the portal-only
evaluation record.

Editor must resolve **PdfTool** and **LoupePreflightPlugin** without a
developer toolchain on PATH.

The profile directory is set by `LOUPE_PREFLIGHT_PROFILES_DIR` in
`LoupeEditorPlugins/LoupePreflightPlugin/CMakeLists.txt`, derived from
`LOUPE_INSTALL_SHARE_DIR`. Note that `-DLOUPE_INSTALL_TO_USR=ON` (used by the
Windows CI and MSI builds) prefixes that with `usr/`, so the shipped path is
`usr/share/loupe/profiles/`. Any smoke test must derive this from the install
prefix rather than assuming a fixed absolute location.

## V1 slim distribution

When `LOUPE_LOUPE_DISTRIBUTION=ON`, prefer Editor + PdfTool + core plugins
(LoupePreflight and required inspection plugins). PageMaster / Diff / Viewer /
LaunchPad may ship in full packages; still build them in CI on both supported OS.

## Cross-platform compatibility pass

Run this pass before marketing V1 on any platform. Goal: correct **dependency
bundling** and **installer packaging** for modules that are already complete.

### Module checklist

| Module / surface | Already complete? | Win | Linux | What to verify |
|------------------|-------------------|:--:|:-----:|----------------|
| LoupeLibCore | Yes | ☐ | ☐ | Qt 6.11.1 + vcpkg build; codecs/fonts; no Widgets |
| LoupeLibWidgets / LoupeLibGui | Yes | ☐ | ☐ | Plugin relative path; settings paths |
| PdfTool (`preflight`, `add-bleed`, …) | Yes | ☐ | ☐ | Bundled next to Editor; working directory; offscreen CI |
| LoupePreflightPlugin | Yes | ☐ | ☐ | Finds PdfTool + `loupe-default.json`; `.dll` / `.so` |
| LoupeEditor | Yes | ☐ | ☐ | Clean-machine launch; plugins load; operator loop |
| Other Editor plugins | Yes | ☐ | ☐ | Present in intended bundle set; load without system Qt |
| LoupePageMaster export (MIC-307–312) | Yes | ☐ | ☐ | Atomic write + manifest; cancel; case-sensitive FS |
| LoupeViewer / Diff / LaunchPad | Adjacent | ☐ | ☐ | Build in CI; optional in slim package |
| loupe-preflight profiles + schemas | Yes | ☐ | ☐ | Installed at documented path; schema version contract |
| UnitTests (operator, corpus, PageMaster) | Yes | ☐ | ☐ | `ctest` green on both CI runners |
| Windows MSI | Session 07 exact-SHA package boundary | ☐ | — | x64 WiX package, dependency evidence, clean VM operator/a11y loop; **V1 ships unsigned** (MIC-342 / MIC-345) |
| Linux AppImage | Session 07 exact-SHA package boundary | — | ☐ | x86_64 package, dependency evidence, clean VM operator/a11y loop |
| Flatpak / MSIX / portable ZIP | Out of Session 07 scope | — | — | Build or sandbox work may exist, but these formats are not release-gate evidence |
| Sentry (optional) | Partial | ☐ | ☐ | Opt-in DSN only; DB path; no default PII |
| OCR sidecar (optional) | Not V1-gated | ☐ | ☐ | Bundled-only guidance; do not block platform gate |
| OcrPlugin (Editor UI) | **Not shipped in V1 — CLI-only, MIC-343** | ☐ | ☐ | `pdfplugins/OcrPlugin.dll` (or `.so`) must be **absent**. Built with `-DLOUPE_PLUGIN_OCR=OFF`; `PdfTool ocr` is unaffected and remains available |

### Bundling rules (all OS)

1. Ship Qt runtime and required Qt plugins (`platforms`, `imageformats`, …) inside the package — do not require a system Qt install.
2. Co-locate PdfTool, LoupePreflightPlugin, and `loupe-default.json` per the layout table.
3. Keep the default bundle C++/Qt only (see `docs/PACKAGING_LICENSING.md`); scan for forbidden Ghostscript / JRE / Python payloads.
4. **V1 ships unsigned** on Windows (no Authenticode). Publish `SHA256SUMS.txt` and disclose SmartScreen (MIC-342). Code signing is post-V1 / paid-distribution (MIC-345) — not a V1 bundling gate.
5. Document upgrade, uninstall, and binary rollback (no cloud DB).
6. OCR ships CLI-only in V1 (`PdfTool ocr`); `OcrPlugin` (the Editor UI plugin) must not be present in any release package format. `scripts/smoke-test-install.ps1` fails the scan if it finds one (MIC-343).
7. Session 07 package evidence must bind the artifact to the full `source_sha`, hash every payload file, inspect every ELF/PE image with the platform tools, record package-contained transitive dependencies and external system dependencies, and fail on any `Qt6Widgets` payload/import/plugin or unresolved non-system dependency.

### Smoke test (every installer)

1. Clean machine (no Qt / MSVC required at runtime).
2. Install the platform package.
3. Launch LoupeEditor.
4. Open a sample PDF; run Loupe Preflight; confirm findings JSON.
5. Confirm PdfTool exists beside the app and profiles resolve.
6. Run `LoupeEditor --quick-smoke` with both the native/default and software Quick backends while developer Qt paths are scrubbed.
7. Archive the package-boundary inspector evidence, smoke transcript, tool versions, file inventory, dependency graph, runtime plugin candidates, and package/source digests. This evidence is qualification data, not a release asset.

Windows automation for steps 2–5 lives in `scripts/smoke-test-install.ps1`; the full
MSI lifecycle wrapper is `scripts/Invoke-MsiSmokeTest.ps1`.
The Linux package equivalent is `scripts/smoke-test-appimage.sh`; pass an explicit
external `TestPdf` on a clean VM. The Session 07 workflows upload evidence under
`loupe-package-boundary-linux-evidence` and `loupe-package-boundary-windows-evidence`.

### Windows MSI clean-machine run (MIC-301)

Prerequisites: a fresh Windows VM with **no** MSVC, Qt, or Python, and an elevated
PowerShell. Build the MSI first via the `Windows_MSI` workflow (`workflow_dispatch`).

```powershell
# Fresh install -> smoke -> uninstall
.\scripts\Invoke-MsiSmokeTest.ps1 `
    -MsiPath .\mberrys.Loupe-pdf_<version>.msi `
    -SourceSha <accepted-session-06-sha> `
    -TestPdf C:\Temp\external-test.pdf

# Add upgrade coverage when a previous MSI is available
.\scripts\Invoke-MsiSmokeTest.ps1 `
    -MsiPath .\mberrys.Loupe-pdf_<new>.msi `
    -PreviousMsiPath .\mberrys.Loupe-pdf_<old>.msi `
    -SourceSha <accepted-session-06-sha> `
    -TestPdf C:\Temp\external-test.pdf
```

The wrapper asserts install, layout resolution, preflight execution, Editor launch,
forbidden-payload absence, upgrade, and clean uninstall. Attach the transcript to
MIC-301.

The MSI workflow now invokes WiX with `Platform=x64` and `-arch x64`; the smoke
wrapper rejects an install directory under `Program Files (x86)` and expects the
64-bit `Program Files` directory. This is a packaging contract, not clean-VM proof.

The clean-VM acceptance gate remains:

1. Use the exact `source_sha` accepted by Session 06 and record it in the MSI/AppImage
   evidence pair.
2. Install from a fresh Windows VM with no MSVC, Qt, Python, or repository checkout.
3. Launch, open the explicit external PDF, navigate workspaces, inspect preflight
   findings, verify names/roles/focus/status behavior, and exit cleanly.
4. Attach the package/dependency evidence and operator/accessibility transcript before
   treating the Session 07 exit gate as passed.

**The profiles path is still a required observation:** `smoke-test-install.ps1` probes
several layouts because `LOUPE_INSTALL_TO_USR=ON` shifts the share tree. Record which
candidate resolved and keep the layout table above synchronized with the VM evidence.

## macOS (post-V1)

macOS is explicitly **out of scope for V1**. The work below is retained as the
entry criteria for adding it in a later release, not as a V1 checklist.

- Apps already set `MACOSX_BUNDLE ON` for Editor, Viewer, PageMaster, Diff, LaunchPad.
- CMake today treats non-`LOUPE_LINUX` like Windows for `LOUPE_PLUGINS_DIR` (`pdfplugins`, `CMakeLists.txt:198-201`). That path must be confirmed inside a `.app` bundle or the install rules adjusted.
- A `macos` job in `ci.yml` with Qt 6.11.1 + vcpkg, mirroring the Ubuntu/Windows `ctest` set, is the minimum bar before any macOS claim is restored.
- Notarization and staple steps belong in a dedicated `macOSInstall.yml` before attaching artifacts to the release draft. This requires an **Apple Developer Program** enrollment, which is not currently held.

## Documentation map

| Doc | Role |
|-----|------|
| This file | Platform policy + compatibility pass |
| `docs/FLATPAK_SANDBOX.md` | Flatpak permissions, named-path overrides, and portal evaluation |
| `README.md` | User-facing supported platforms and install pointers |
| `docs/CI.md` | Which OS jobs run in GitHub Actions |
| `docs/PACKAGING_LICENSING.md` | License / SBOM / default-bundle rules |
| `docs/SESSION_07_PACKAGE_BOUNDARY.md` | Exact-SHA package evidence and clean-VM gate |
| `docs/PRODUCTION_RUNBOOK.md` | Deploy / rollback / support |
| `AGENTS.md` | Contributor note: plugin dirs per OS |
