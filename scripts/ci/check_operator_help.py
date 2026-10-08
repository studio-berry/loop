#!/usr/bin/env python3
"""Check the shipped catalog binding and licensed deterministic learning corpus."""

from __future__ import annotations

import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from scripts.ci.generate_operator_samples import generated_files


ROOT = Path(__file__).resolve().parents[2]


def check(root: Path = ROOT, expected: dict[str, bytes] | None = None) -> list[str]:
    errors = []
    qrc = root / "LoopEditor/app.qrc"
    resources = {entry.attrib.get("alias", entry.text): (qrc.parent / entry.text).resolve()
                 for entry in ET.parse(qrc).iter("file")}
    if resources.get("operator/check-catalog.json") != (root / "docs/generated/preflight-check-catalog.json").resolve():
        errors.append("Operator help must consume the generated check catalog directly")
    catalog = json.loads((root / "docs/generated/preflight-check-catalog.json").read_text(encoding="utf-8"))
    for check_id in catalog["registry"]:
        item = catalog["checks"].get(check_id, {})
        if any(not item.get(field) for field in ("coverage", "measures", "limitations", "severity")):
            errors.append(f"Missing operator help for registered check: {check_id}")
    guide = json.loads((root / "LoopEditor/operator/guide.json").read_text(encoding="utf-8"))
    if [item.get("name") for item in guide["workspaces"]] != ["Document", "Preflight", "Production Preview", "Pages", "Inspect", "Fix", "Compare"]:
        errors.append("All seven workspaces need ordered help")
    if any(not item.get("help") for item in guide["workspaces"]) or len(guide.get("getting_started", [])) < 6:
        errors.append("Workspace help or onboarding steps are missing")
    expected = generated_files() if expected is None else expected
    for name, data in expected.items():
        path = root / "LoopEditor/operator/samples" / name
        if not path.is_file() or path.read_bytes() != data:
            errors.append(f"Sample member differs from its deterministic source: {name}")
        if resources.get("operator/samples/" + name) != path.resolve():
            errors.append(f"Sample member is not bundled: {name}")
    return errors


def main() -> int:
    try:
        errors = check()
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print(f"Operator help check failed: {error}", file=sys.stderr)
        return 1
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Operator help catalog binding, workspace guide and licensed sample corpus passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
