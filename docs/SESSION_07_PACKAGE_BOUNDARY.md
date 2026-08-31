# Session 07 — package-boundary evidence

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

## Clean-machine final gate

Build-runner smoke and the product Quick accessibility harness are supporting
qualification evidence. Final proof requires disposable clean Ubuntu 24.04 and
Windows Server 2022 machines with no Qt, MSVC, Python, repository checkout, or
developer paths:

1. Install the exact-SHA package under test.
2. Launch the installed Editor and open an explicit external test PDF.
3. Navigate workspaces and inspect preflight state and findings.
4. Verify accessible names, roles, focus movement, status behavior, and clean exit.
5. On Windows, verify the MSI installed under 64-bit `Program Files`; on Linux,
   verify the AppImage runs without a host Qt installation.
6. Record the package digest, source SHA, operator/accessibility transcript, and
   uninstall result. Only then may Issues LOUPE-22, LOUPE-23, LOUPE-24 and the
   Session 07 exit gate be marked complete in Notion.
