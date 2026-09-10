#!/usr/bin/env python3
"""Generate THIRD_PARTY_NOTICES.txt from final package-boundary evidence."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any, Sequence

from package_licensing_common import (
    LicensingError,
    group_components,
    iter_shipped_binaries,
    load_boundary_evidence,
    read_notice_text,
    utc_now,
)


def build_notices(evidence: dict[str, Any]) -> str:
    package = evidence.get("package", {})
    groups = group_components(iter_shipped_binaries(evidence))
    lines = [
        "Loop Third-Party Notices",
        f"Generated: {utc_now()}",
        f"Source SHA: {evidence['source_sha']}",
        f"Package: {package.get('name', 'unknown')} ({package.get('format', 'unknown')})",
        f"Package SHA256: {package.get('sha256', 'unknown')}",
        "",
        "This file is generated from the final packaged artifact payload, not from",
        "vcpkg.json or the build tree alone. See docs/PACKAGING_LICENSING.md.",
        "",
        "=" * 78,
        "SUMMARY",
        "=" * 78,
        "",
    ]

    for group_name, group in sorted(groups.items(), key=lambda item: item[0].lower()):
        component = group["component"]
        artifact_count = len(group["artifacts"])
        lines.append(f"- {group_name} ({component.spdx_id}) — {artifact_count} shipped artifact(s)")

    lines.extend(["", "=" * 78, "LICENSE TEXT", "=" * 78, ""])

    for group_name, group in sorted(groups.items(), key=lambda item: item[0].lower()):
        component = group["component"]
        lines.extend(
            [
                "=" * 78,
                f"{group_name} — {component.spdx_id}",
                "Shipped artifacts:",
            ]
        )
        for artifact in group["artifacts"]:
            lines.append(f"  - {artifact['path']} (sha256 {artifact.get('sha256', 'unknown')})")
        lines.append("")
        notice = read_notice_text(component)
        if notice:
            lines.append(notice.rstrip())
        else:
            lines.append(
                "License text not bundled in-repo for this component. "
                "Obtain the upstream license from the component distributor."
            )
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True, help="package-boundary evidence JSON")
    parser.add_argument("--output", type=Path, required=True, help="THIRD_PARTY_NOTICES.txt output path")
    args = parser.parse_args(argv)
    try:
        evidence = load_boundary_evidence(args.evidence.resolve())
        notices = build_notices(evidence)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(notices, encoding="utf-8")
    except (LicensingError, OSError) as exc:
        print(f"Package notices generation FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "Package notices generated: "
        f"platform={evidence.get('platform')} "
        f"source_sha={evidence.get('source_sha')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
