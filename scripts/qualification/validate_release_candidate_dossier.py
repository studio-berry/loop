#!/usr/bin/env python3
"""Validate release-candidate dossier provenance invariants."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


SHA256_RE = re.compile(r"^[0-9a-f]{40}$")


def validate_dossier(dossier: Any) -> list[str]:
    """Return structural/provenance errors not expressible as schema consts."""

    errors: list[str] = []
    if not isinstance(dossier, dict):
        return ["dossier must be a JSON object"]

    candidate_sha = dossier.get("candidate_sha")
    if not isinstance(candidate_sha, str) or not SHA256_RE.fullmatch(candidate_sha):
        errors.append("candidate_sha must be a 40-character lowercase hexadecimal commit SHA")

    artifacts = dossier.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        errors.append("artifacts must be a non-empty array")
        return errors

    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            errors.append(f"artifacts[{index}] must be an object")
            continue

        built_from_sha = artifact.get("built_from_sha")
        if not isinstance(built_from_sha, str) or not SHA256_RE.fullmatch(built_from_sha):
            errors.append(f"artifacts[{index}].built_from_sha must be a 40-character lowercase hexadecimal commit SHA")
        elif built_from_sha != candidate_sha:
            errors.append(f"artifacts[{index}].built_from_sha must equal candidate_sha")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dossier", type=Path, help="Release-candidate dossier JSON file")
    args = parser.parse_args(argv)

    try:
        dossier = json.loads(args.dossier.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"unable to read dossier: {error}", file=sys.stderr)
        return 2

    errors = validate_dossier(dossier)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print("Release-candidate dossier provenance passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
