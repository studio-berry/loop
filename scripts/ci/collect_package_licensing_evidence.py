#!/usr/bin/env python3
"""Collect Session 13 package-licensing evidence from final-artifact inputs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Sequence

_CI_DIR = Path(__file__).resolve().parent
if str(_CI_DIR) not in sys.path:
    sys.path.insert(0, str(_CI_DIR))

from compare_package_boundary_evidence import compare
from package_licensing_common import LicensingError, load_boundary_evidence, utc_now


FULL_SHA = re.compile(r"^[0-9a-fA-F]{40}$")


def collect(
    linux_evidence: Path,
    windows_evidence: Path,
    source_sha: str,
    linux_sbom: Path | None,
    linux_notices: Path | None,
    windows_sbom: Path | None,
    windows_notices: Path | None,
    linux_relink: Path | None,
    windows_relink: Path | None,
    linux_clean_machine: Path | None,
) -> dict[str, Any]:
    if not FULL_SHA.fullmatch(source_sha):
        raise LicensingError("source_sha must be a full 40-character Git SHA")

    linux = load_boundary_evidence(linux_evidence)
    windows = load_boundary_evidence(windows_evidence)
    pair = compare(linux_evidence, windows_evidence, source_sha)

    artifacts = {
        "linux": {
            "boundary_evidence": linux_evidence.as_posix(),
            "sbom": linux_sbom.as_posix() if linux_sbom else None,
            "third_party_notices": linux_notices.as_posix() if linux_notices else None,
            "qt_relink_transcript": linux_relink.as_posix() if linux_relink else None,
            "clean_machine_smoke": linux_clean_machine.as_posix() if linux_clean_machine else None,
            "package": linux["package"],
        },
        "windows": {
            "boundary_evidence": windows_evidence.as_posix(),
            "sbom": windows_sbom.as_posix() if windows_sbom else None,
            "third_party_notices": windows_notices.as_posix() if windows_notices else None,
            "qt_relink_transcript": windows_relink.as_posix() if windows_relink else None,
            "package": windows["package"],
        },
        "paired_boundary": pair,
    }

    required_paths = [
        linux_sbom,
        linux_notices,
        windows_sbom,
        windows_notices,
        linux_relink,
        windows_relink,
        linux_clean_machine,
    ]
    complete = all(path is not None and path.is_file() for path in required_paths)
    status = "passed" if complete and linux["status"] == "passed" and windows["status"] == "passed" else "incomplete"

    return {
        "schema_version": 1,
        "kind": "loop-package-licensing-evidence",
        "generated_at": utc_now(),
        "source_sha": source_sha.lower(),
        "status": status,
        "session": 13,
        "policy": "docs/PACKAGING_LICENSING.md",
        "procedure": "docs/SESSION_13_PACKAGE_LICENSING.md",
        "artifacts": artifacts,
        "release_gates": {
            "final_artifact_sbom": "complete" if complete else "open",
            "third_party_notices": "complete" if complete else "partial",
            "clean_machine_package_smoke": "complete" if linux_clean_machine and linux_clean_machine.is_file() else "open",
            "qt_relink_test": "complete" if linux_relink and windows_relink and linux_relink.is_file() and windows_relink.is_file() else "open",
        },
        "known_limitations": [
            "Windows Server 2022 pristine VM clean-machine proof remains deferred to 1.0 per Session 07.",
            "Session 07 package evidence on b47c62b2 does not transfer; all artifacts must bind to this source_sha.",
        ],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-evidence", type=Path, required=True)
    parser.add_argument("--windows-evidence", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--linux-sbom", type=Path)
    parser.add_argument("--linux-notices", type=Path)
    parser.add_argument("--windows-sbom", type=Path)
    parser.add_argument("--windows-notices", type=Path)
    parser.add_argument("--linux-relink", type=Path)
    parser.add_argument("--windows-relink", type=Path)
    parser.add_argument("--linux-clean-machine", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        evidence = collect(
            args.linux_evidence.resolve(),
            args.windows_evidence.resolve(),
            args.source_sha,
            args.linux_sbom.resolve() if args.linux_sbom else None,
            args.linux_notices.resolve() if args.linux_notices else None,
            args.windows_sbom.resolve() if args.windows_sbom else None,
            args.windows_notices.resolve() if args.windows_notices else None,
            args.linux_relink.resolve() if args.linux_relink else None,
            args.windows_relink.resolve() if args.windows_relink else None,
            args.linux_clean_machine.resolve() if args.linux_clean_machine else None,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    except (LicensingError, OSError, ValueError) as exc:
        print(f"Package licensing evidence collection FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "Package licensing evidence collected: "
        f"source_sha={evidence['source_sha']} status={evidence['status']}"
    )
    return 0 if evidence["status"] == "passed" else 2


if __name__ == "__main__":
    raise SystemExit(main())
