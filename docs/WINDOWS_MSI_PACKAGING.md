# Windows MSI packaging — regression traps

**Read this before changing Windows packaging, WiX, relocated smoke, or the
package-boundary inspector.**

The `Windows_MSI` workflow is a three-stage gate:

1. **Relocated install smoke** — `scripts/smoke-test-install.ps1` on the staged
   `cmake --install` tree with developer Qt paths scrubbed.
2. **MSI build** — WiX `heat` / `candle` / `light` on that tree.
3. **Package-boundary inspection** — `scripts/ci/inspect_package_dependencies.py`
   on the produced MSI.

A green build requires all three. Fixing stage 1 while breaking stage 2 or 3
was the dominant failure mode during the 2026 relocated-smoke recovery (PR #532).

## Mandatory packaging steps (do not remove)

These steps are enforced by `scripts/ci/test_workflow_contracts.py`. If you
relocate or refactor `WindowsInstall.yml`, preserve the contracts below.

| Step | Script / location | Why |
|------|-------------------|-----|
| Prune optional SQL drivers | `scripts/prepare-windows-install.ps1` (before MSI) | Qt deploy copies vendor SQL plugins (`qsqlibase`, `qsqloci`, …) that import client libraries we do not ship. Linux does the same in `scripts/prepare-appdir.sh` (keep **QSQLITE only**). |
| Bind WiX harvest paths | `heat … -var var.SourceDir` and `candle … -dSourceDir=$installDir` on `HarvestedInstall.wxs` | Without this, `light` resolves harvested files under `loop/build/WixInstaller/SourceDir/` instead of `loop/build/install/` (`LGHT0103`). |
| Harvest full install tree | `heat dir …/build/install` with `-dr INSTALLFOLDER` | The MSI must match `cmake --install` output, not hand-maintained WiX file lists. |
| Inspect the MSI artifact | `scripts/ci/inspect_package_dependencies.py --platform windows` | Final proof is the packaged MSI, not the staged tree. |

## Failure signatures and fixes

### `LGHT0103: The system cannot find the file 'SourceDir\…'`

**Cause:** `heat` emitted relative `Source="…"` paths and `light` bound them to
the WiX working directory instead of the install tree.

**Fix:** Pass `-var var.SourceDir` to `heat` and define `SourceDir` when
compiling `HarvestedInstall.wxs`. See the `Create MSI Package` step in
`.github/workflows/WindowsInstall.yml`.

### `Package boundary inspection failed` with `unresolved-dependency`

**Cause A — optional SQL drivers still in the MSI:** dumpbin sees imports like
`fbclient.dll`, `OCI.dll`, `LIBPQ.dll` from Qt SQL plugins.

**Fix:** Run `prepare-windows-install.ps1` before `heat`. Do not ship non-QSQLITE
drivers in release packages.

**Cause B — Windows platform APIs treated as external:** Qt platform plugins and
core libraries import OS DLLs (DirectWrite, DXGI, WinHTTP, ODBC, ICU, …) that are
not in the package but are supplied by Windows.

**Fix:** Add the DLL to `windows_system_dependency()` in
`scripts/ci/inspect_package_dependencies.py` when dumpbin shows a genuine Windows
system API. Do **not** silence vendor/client libraries (Firebird, Oracle, …) this
way — prune the plugin instead.

### Relocated smoke `0xC0000005` (access violation)

**Cause:** MSVC DLL-boundary mistakes during packaged startup or teardown.

**Do not regress:**

| Area | Rule |
|------|------|
| Qt plugin discovery | On Windows, resolve `install-root/plugins/` only. **Never** add `usr/bin` to `QCoreApplication::libraryPaths()` — Qt treats product DLLs as plugins and faults during QPA startup. |
| PdfTool preflight | Run inspection inside LoopLibCore (`inspectPreflightFile`). Use `createForInspection()` sessions that skip `PDFResourceBudget` / `PDFPageCacheBudget`. Exit preflight with `_Exit(0)` on Windows to avoid MSVC teardown faults across the DLL boundary. |
| LoopEditor `--quick-smoke` | Finish init ordering before `PDFDocumentSession` / page-cache wiring; avoid cross-DLL `PDFPageCacheBudget` reads; call `std::_Exit(0)` after scene-graph success once startup is proven. |
| Packaged fonts / offscreen | Ship Liberation fonts under `usr/lib/fonts`; stage `qoffscreen` for headless QPA; set `Plugins = plugins` in `qt.conf` when windeployqt omits it. |

## WiX source hygiene

- `WixInstaller/Product.wxs.in` is XML. Comments must not contain `--` (invalid
  in XML comments) or `candle` fails (`CNDL0104`).
- The installed-tree harvest replaces hand-maintained file lists in
  `WixInstaller/CMakeLists.txt`. Do not reintroduce Qt-SDK file discovery there.

## When you change this area

1. Read this document and `docs/SESSION_07_PACKAGE_BOUNDARY.md`.
2. Run `python3 -m unittest scripts.ci.test_workflow_contracts -v`.
3. Run `python3 -m unittest scripts.ci.test_inspect_package_dependencies -v`.
4. If you touch smoke or preflight paths, expect a full `Windows_MSI` CI run (~50
   minutes) before calling packaging proven.

## Related files

| File | Role |
|------|------|
| `.github/workflows/WindowsInstall.yml` | Build, smoke, MSI, inspection |
| `scripts/prepare-windows-install.ps1` | Pre-MSI install-tree prune |
| `scripts/prepare-appdir.sh` | Linux equivalent SQL-driver prune |
| `scripts/smoke-test-install.ps1` | Relocated install smoke |
| `scripts/ci/inspect_package_dependencies.py` | MSI/AppImage boundary inspector |
| `WixInstaller/Product.wxs.in` | WiX product shell + harvest ref |
