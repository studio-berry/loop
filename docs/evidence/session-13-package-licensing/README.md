# Session 13 package-licensing evidence

Frozen evidence for Issues 40–42 (P-02 + P-01 SHA re-proof).

**Status:** `incomplete` until hosted package workflows run on the exact candidate
SHA and all required artifacts are copied here.

## Required artifacts (per platform)

| Artifact | Linux | Windows |
| --- | --- | --- |
| Package-boundary evidence | `linux-evidence.json` | `windows-evidence.json` |
| SPDX SBOM | `linux-components.spdx.json` | `windows-components.spdx.json` |
| Third-party notices | `linux-THIRD_PARTY_NOTICES.txt` | `windows-THIRD_PARTY_NOTICES.txt` |
| Qt relink transcript | `linux-qt-relink.txt` | `windows-qt-relink.txt` |
| Clean-machine smoke | `linux-clean-machine-smoke.txt` | workflow transcript (hosted MSI smoke) |

## Paired proof

- `paired-evidence.json` — output of `compare_package_boundary_evidence.py`
- `evidence.json` — Session 13 manifest from `collect_package_licensing_evidence.py`

## Procedure

See `docs/SESSION_13_PACKAGE_LICENSING.md`.

## Session 07 note

Evidence under `docs/evidence/session-07-package-boundary/` remains historical
qualification on `b47c62b2…` and does **not** satisfy Session 13.
