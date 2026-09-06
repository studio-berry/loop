# Session 07 — package-boundary evidence

> **Accepted result (2026-09-06):** Both Linux AppImage and Windows MSI qualified from the
> same exact source SHA **`b47c62b263a3fd7fb36940856866e589bbc8be10`** (the merged PR #532
> head on `dev`). See `docs/evidence/session-07-package-boundary/`.

Session 07 qualifies only the Linux x86_64 AppImage and Windows x64 MSI. Flatpak,
MSIX, and portable ZIP are outside this gate. The MSI is unsigned for this session;
signing is not an acceptance requirement.

## Source and workflow contract

The package workflows require a full 40-character `source_sha`. They check out that
commit, verify `git rev-parse HEAD`, and write the verified value into the evidence
and smoke transcript. Dispatch both workflows with the same accepted Session 06 SHA:

```text
source_sha=<accepted-session-06-output-sha>
```

The release-draft workflow checks both successful package runs against that input,
downloads their evidence artifacts, and runs:

```text
python3 scripts/ci/compare_package_boundary_evidence.py \
  --linux package-evidence/linux/evidence.json \
  --windows package-evidence/windows/evidence.json \
  --source-sha <accepted-session-06-output-sha>
```

Package-boundary evidence is uploaded as CI evidence and is removed from release
asset staging. It must not be treated as publication proof by itself.

## Inspector interface

The platform-aware inspector extracts the artifact unless `--payload-root` is used
for a fixture or a re-check:

```text
python3 scripts/ci/inspect_package_dependencies.py \
  --platform linux|windows \
  --package <AppImage-or-MSI> \
  --source-sha <full-sha> \
  --expected-architecture x86-64|x64 \
  --work-dir <isolated-extraction-dir> \
  --output <evidence.json> \
  --report <inspection.txt>
```

The versioned evidence document records artifact identity, the complete hashed file
inventory, every ELF/PE binary, architecture, direct imports, package-contained
dependency closure, external system dependencies, runtime plugin candidates,
inspection-tool versions, forbidden findings, and check results. Missing tools,
unknown binaries/architectures, Qt6Widgets payloads or imports, and unresolved
non-system dependencies fail closed.

## 0.2.0 gate hierarchy

The goal of the fresh-environment checking is the isolation property: Loop must not
depend on the build/development machine having Qt, Visual Studio, DLLs, plugins, PATH
entries, or other undeclared state. Windows Server 2022 is not itself a requirement;
isolation is. For 0.2.0 the gate is structured as:

| Validation | 0.2.0 |
| --- | --- |
| Clean CI checkout/build | **Required** |
| Windows packaging succeeds | **Required** |
| Packaged app launches outside the build tree | **Required** |
| Functional PDF smoke (open → render → preflight → save/export → reopen) | **Required** |
| Dependency/package inspection | **Required** |
| Separate ordinary Windows PC test | Recommended |
| Completely fresh Windows VM | Recommended |
| Windows Server 2022 pristine VM | **Defer** (release-hardening, required before 1.0) |

"Builds correctly on Windows" is necessary but not sufficient alone: a developer
workstation is contaminated as a deployment test because it carries SDKs and dev tooling,
so a successful build proves compilation, not redistribution. The required 0.2.0 evidence
must come from the packaged/installed artifacts outside the build tree, not from the
source directory.

### Required 0.2.0 evidential checks (configured in the package workflows)

Against each exact-SHA package (Linux AppImage and Windows MSI), from the installed
artifacts and not the build tree:

1. Install/launch the exact-SHA package under test; on Windows verify the MSI installs
   under 64-bit `Program Files`.
2. Launch the packaged Editor from its installed location (`--quick-smoke` native +
   software) and open an explicit external test PDF (operator launch stays alive).
3. Run PdfTool preflight against the test PDF.
4. Run the product Quick accessibility harness native + software lanes.
5. Dependency/package inspection (`inspect_package_dependencies.py`) with no
   Qt6Widgets payload/import, no unresolved non-system dependency, no forbidden
   payload (Ghostscript / JRE / Python), and no Widgets-bound Qt module.

### Clean-machine qualification record (2026-09-06)

- **Linux:** the exact-SHA AppImage passed the full smoke (`smoke-test-appimage.sh
  --operator`) in a disposable clean Ubuntu 24.04 container with no Qt/MSVC/Python/dev
  paths — PdfTool preflight, editor native+software Quick startup, operator launch
  alive, no Widgets/GS/JRE/Python payload. See
  `docs/evidence/session-07-package-boundary/linux-clean-machine-smoke.txt`.
- **Windows:** the exact-SHA MSI passed the Windows_MSI workflow (relocated
  installed-tree smoke, MSI lifecycle, package inspection, a11y native+software) on
  `34050832684`. A **Windows Server 2022 pristine-VM run is deferred** to release
  hardening (required before 1.0) and is **not a blocker for 0.2.0**.

Record the package digest, source SHA, operator/accessibility transcript, and uninstall
result with the evidence. Issues LOUPE-22 and LOUPE-23 are complete; LOUPE-24 is complete
for the required 0.2.0 lanes, with the pristine-VM hardening item tracked separately.

## Windows MSI regression traps

The `Windows_MSI` workflow fails in three independent stages (relocated smoke,
WiX MSI build, package-boundary inspection). **Read
`docs/WINDOWS_MSI_PACKAGING.md` before changing WiX, install staging, relocated
smoke, or the boundary inspector** — that document lists mandatory workflow
steps, failure signatures, and MSVC/Qt pitfalls that must not be reintroduced.
