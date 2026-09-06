# Session 13 — package and licensing qualification

Session 13 closes P-02 (supply chain/licensing) and re-proves P-01 package
identity on the **exact candidate SHA**. Session 07 evidence on
`b47c62b263a3fd7fb36940856866e589bbc8be10` does **not** transfer.

## Exit gate

- P-02 complete for exact final artifacts: artifact-derived SBOM,
  `THIRD_PARTY_NOTICES.txt`, LGPL relink/replace evidence, and archived
  corresponding-source or written-offer record.
- P-01 re-proof on the same candidate SHA via paired package-boundary evidence
  and clean-machine smoke (Linux required; Windows Server 2022 pristine VM
  remains deferred per Session 07).

Do not mark gates `complete` without hosted package artifacts built from the
candidate SHA.

## Candidate SHA discipline

1. Record the Session 09+ merged `candidate_sha` (40-char lowercase hex).
2. Dispatch **both** package workflows with `source_sha=<candidate_sha>`:
   - `Linux_AppImage`
   - `Windows_MSI`
3. Download evidence artifacts and run the pairing/comparator steps below.
4. If any qualification lane lands code after packaging, **re-run Session 13**
   on the new SHA before Session 14.

## Package identity workflow

Follow `docs/SESSION_07_PACKAGE_BOUNDARY.md` for the inspector contract.
Session 13 adds final-artifact licensing outputs on top of the same boundary
evidence.

### 1. Dispatch exact-SHA package builds

```text
workflow: Linux_AppImage
input: source_sha=<candidate_sha>

workflow: Windows_MSI
input: source_sha=<candidate_sha>
```

Both workflows verify checkout SHA, record `LOOP_SOURCE_SHA`, run package-boundary
inspection, and (after this session) emit SBOM, notices, and Qt relink
transcripts into the evidence artifact bundle.

### 2. Pair Linux and Windows boundary evidence

```text
python3 scripts/ci/compare_package_boundary_evidence.py \
  --linux package-evidence/linux/evidence.json \
  --windows package-evidence/windows/evidence.json \
  --source-sha <candidate_sha> \
  --output docs/evidence/session-13-package-licensing/paired-evidence.json
```

### 3. Generate artifact-derived SBOM and notices

From each platform's `evidence.json` (final packaged payload, not vcpkg tree):

```text
python3 scripts/ci/generate_package_sbom.py \
  --evidence package-evidence/linux/evidence.json \
  --output docs/evidence/session-13-package-licensing/linux-components.spdx.json

python3 scripts/ci/generate_package_third_party_notices.py \
  --evidence package-evidence/linux/evidence.json \
  --output docs/evidence/session-13-package-licensing/linux-THIRD_PARTY_NOTICES.txt
```

Repeat for Windows with `windows-evidence.json` and `windows-*` output names.

### 4. LGPL Qt relink/replace test

Linux AppImage payload:

```text
bash scripts/ci/run_qt_relink_test.sh \
  /path/to/Loop-pdf-VERSION-x86_64.AppImage \
  --output docs/evidence/session-13-package-licensing/linux-qt-relink.txt
```

Windows installed tree (after MSI install under 64-bit Program Files):

```powershell
.\scripts\ci\run_qt_relink_test.ps1 `
  -InstallDir "C:\Program Files\LOOP" `
  -SourceSha <candidate_sha> `
  -OutputPath docs\evidence\session-13-package-licensing\windows-qt-relink.txt
```

### 5. Clean-machine smoke

| Platform | 0.2.0 requirement | Procedure |
| --- | --- | --- |
| Linux | **Required** | Disposable Ubuntu 24.04 container with no Qt/MSVC/Python/dev paths. Run `scripts/smoke-test-appimage.sh <AppImage> --operator`. Archive transcript to `linux-clean-machine-smoke.txt`. |
| Windows hosted MSI | **Required** | `Invoke-MsiSmokeTest.ps1` on the workflow runner against the exact-SHA MSI (packaged launch outside build tree). Evidence uploaded by `Windows_MSI`. |
| Windows Server 2022 pristine VM | **Deferred to 1.0** | Document as known limitation; not a 0.2.0 blocker. |

Example Linux clean-machine container pattern (from Session 07):

```text
docker run --rm -v "$PWD:/work" -w /work ubuntu:24.04 bash -lc '
  apt-get update && apt-get install -y libxcb-cursor0 libfontconfig1 libglib2.0-0 libdbus-1-3
  LOOP_SOURCE_SHA=<candidate_sha> bash scripts/smoke-test-appimage.sh /work/Loop-pdf-*.AppImage --operator
'
```

### 6. Freeze Session 13 evidence manifest

```text
python3 scripts/ci/collect_package_licensing_evidence.py \
  --linux-evidence docs/evidence/session-13-package-licensing/linux-evidence.json \
  --windows-evidence docs/evidence/session-13-package-licensing/windows-evidence.json \
  --source-sha <candidate_sha> \
  --linux-sbom docs/evidence/session-13-package-licensing/linux-components.spdx.json \
  --linux-notices docs/evidence/session-13-package-licensing/linux-THIRD_PARTY_NOTICES.txt \
  --windows-sbom docs/evidence/session-13-package-licensing/windows-components.spdx.json \
  --windows-notices docs/evidence/session-13-package-licensing/windows-THIRD_PARTY_NOTICES.txt \
  --linux-relink docs/evidence/session-13-package-licensing/linux-qt-relink.txt \
  --windows-relink docs/evidence/session-13-package-licensing/windows-qt-relink.txt \
  --linux-clean-machine docs/evidence/session-13-package-licensing/linux-clean-machine-smoke.txt \
  --output docs/evidence/session-13-package-licensing/evidence.json
```

`collect_package_licensing_evidence.py` exits `0` only when `status` is
`passed` (all required artifacts present and boundary evidence passed).

### 7. Update release gates and closeout matrix

When `evidence.json` reports `status: passed`:

- Set `docs/quick-runtime-manifest.json` `release_gates` to `complete` with
  evidence pointers (or `partial` until all lanes are green).
- Update `docs/0.2.0-closeout-matrix.md` P-02 to `acceptance verified` and
  refresh P-01 SHA binding to `<candidate_sha>`.

## Corresponding source / written offer

Per `docs/PACKAGING_LICENSING.md`, archive the Qt corresponding-source archive
or valid written offer under Berry Studio control. Record the location in the
Session 13 evidence bundle (`written-offer.txt` or equivalent) — not in release
assets.

## Tooling map

| Tool | Purpose |
| --- | --- |
| `scripts/ci/inspect_package_dependencies.py` | Final-artifact dependency graph |
| `scripts/ci/compare_package_boundary_evidence.py` | Paired Linux/Windows SHA proof |
| `scripts/ci/generate_package_sbom.py` | SPDX 2.3 SBOM from boundary evidence |
| `scripts/ci/generate_package_third_party_notices.py` | Notices from shipped payload |
| `scripts/ci/run_qt_relink_test.sh` / `.ps1` | LGPL relink evidence |
| `scripts/ci/collect_package_licensing_evidence.py` | Session evidence manifest |
| `scripts/generate-third-party-notices.ps1` | Legacy vcpkg-tree notices (partial only) |

## Related issues

- Issue 40 — final-artifact SBOM, notices, LGPL evidence
- Issue 41 — package identity and clean-machine lifecycle
- Issue 42 — close P-01/P-02 with final-artifact evidence
