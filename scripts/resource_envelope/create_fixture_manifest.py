#!/usr/bin/env python3
"""Create a provenance manifest for external resource-envelope fixtures."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

from scripts.resource_envelope.run_matrix import FIXTURE_SPECS, _fixture_args, _sha256


def create_manifest(fixtures: dict[str, Path], provenance: str) -> dict[str, object]:
    records: list[dict[str, object]] = []
    for fixture_id, path in fixtures.items():
        if not path.is_file():
            raise ValueError(f"fixture not found: {fixture_id}: {path}")
        record: dict[str, object] = {
            "fixture_id": fixture_id,
            "path": str(path.resolve()),
            "sha256": _sha256(path),
            "size_bytes": path.stat().st_size,
            "provenance": provenance,
        }
        expected_page_count = FIXTURE_SPECS[fixture_id]["expected_page_count"]
        if expected_page_count is not None:
            record["page_count"] = expected_page_count
        records.append(record)
    return {
        "schema_kind": "loop-resource-envelope-fixtures",
        "schema_version": 1,
        "fixtures": records,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--provenance", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        manifest = create_manifest(_fixture_args(args.fixture), args.provenance)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as exc:
        print(f"resource-envelope manifest error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"output": str(args.output.resolve()), "fixtures": len(manifest["fixtures"])}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
