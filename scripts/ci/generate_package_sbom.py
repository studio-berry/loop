#!/usr/bin/env python3
"""Generate an SPDX 2.3 SBOM from final package-boundary evidence."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Sequence

from package_licensing_common import (
    LicensingError,
    group_components,
    iter_shipped_binaries,
    load_boundary_evidence,
    spdx_ref,
    utc_now,
)


def build_sbom(evidence: dict[str, Any]) -> dict[str, Any]:
    platform = str(evidence.get("platform", "unknown"))
    source_sha = str(evidence["source_sha"]).lower()
    package = evidence.get("package", {})
    binaries = iter_shipped_binaries(evidence)
    groups = group_components(binaries)

    document_name = f"Loop-pdf-{platform}-package-sbom"
    namespace = f"https://github.com/studio-berry/loop/spdx/{source_sha}/{platform}"

    packages: list[dict[str, Any]] = [
        {
            "name": str(package.get("name", "Loop package")),
            "SPDXID": "SPDXRef-Package",
            "versionInfo": "NOASSERTION",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": True,
            "checksums": [
                {
                    "algorithm": "SHA256",
                    "checksumValue": str(package.get("sha256", "")),
                }
            ],
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "externalRefs": [
                {
                    "referenceCategory": "PACKAGE-MANAGER",
                    "referenceType": "purl",
                    "referenceLocator": f"pkg:github/studio-berry/loop@{source_sha}",
                }
            ],
        }
    ]
    relationships: list[dict[str, Any]] = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": "SPDXRef-Package",
        }
    ]

    for group_name, group in sorted(groups.items(), key=lambda item: item[0].lower()):
        component = group["component"]
        ref = spdx_ref(group_name)
        artifact_paths = [str(item["path"]) for item in group["artifacts"]]
        packages.append(
            {
                "name": group_name,
                "SPDXID": ref,
                "versionInfo": "NOASSERTION",
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "licenseConcluded": component.spdx_id,
                "licenseDeclared": component.spdx_id,
                "copyrightText": "NOASSERTION",
                "comment": "Shipped in final package payload: " + ", ".join(artifact_paths[:8])
                + (" ..." if len(artifact_paths) > 8 else ""),
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-Package",
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": ref,
            }
        )

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": document_name,
        "documentNamespace": namespace,
        "creationInfo": {
            "created": utc_now(),
            "creators": ["Tool: loop-generate-package-sbom"],
            "comment": (
                "Generated from loop-package-boundary-evidence for the final packaged "
                "artifact, not from the vcpkg tree alone."
            ),
        },
        "documentDescribes": ["SPDXRef-Package"],
        "packages": packages,
        "relationships": relationships,
        "annotations": [
            {
                "annotationDate": utc_now(),
                "annotationType": "OTHER",
                "annotator": "Tool: loop-generate-package-sbom",
                "comment": (
                    f"source_sha={source_sha}; platform={platform}; "
                    f"binary_count={len(binaries)}; component_count={len(groups)}"
                ),
            }
        ],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True, help="package-boundary evidence JSON")
    parser.add_argument("--output", type=Path, required=True, help="SPDX JSON output path")
    args = parser.parse_args(argv)
    try:
        evidence = load_boundary_evidence(args.evidence.resolve())
        sbom = build_sbom(evidence)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8")
    except (LicensingError, OSError) as exc:
        print(f"Package SBOM generation FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "Package SBOM generated: "
        f"platform={evidence.get('platform')} "
        f"source_sha={evidence.get('source_sha')} "
        f"components={len(sbom['packages']) - 1}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
