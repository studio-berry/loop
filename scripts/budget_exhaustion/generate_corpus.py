#!/usr/bin/env python3
"""Generate the deterministic synthetic processing-budget exhaustion corpus."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


DEFAULT_OUTPUT = Path(__file__).resolve().parents[2] / "UnitTests" / "testdata" / "budget-exhaustion"
SCHEMA_KIND = "loupe-processing-budget-exhaustion-corpus"
SCHEMA_VERSION = 1


def _fixture(
    fixture_id: str,
    operation: str,
    kind: str,
    pool: str,
    limit: int,
    attempted: int,
    context: str,
    **payload: int,
) -> dict[str, object]:
    return {
        "id": fixture_id,
        "operation": operation,
        "budget_kind": kind,
        "budget_pool": pool,
        "limit": limit,
        "attempted": attempted,
        "context": context,
        "payload": payload,
    }


def fixtures() -> list[dict[str, object]]:
    """Return all budget dimensions in stable order.

    The payloads are deliberately small. They describe the bounded synthetic
    work the C++ harness performs; they are not third-party samples or large
    binary inputs.
    """

    return [
        _fixture("input-bytes", "charge-input-bytes", "input-bytes", "document-model", 64, 65, "synthetic/input-bytes", source_bytes=65),
        _fixture("single-decoded-stream-bytes", "check-decoded-stream-size", "single-decoded-stream-bytes", "decoded-streams", 64, 65, "synthetic/single-decoded-stream-bytes", compressed_bytes=65),
        _fixture("cumulative-decoded-bytes", "charge-decoded-bytes", "cumulative-decoded-bytes", "decoded-streams", 64, 65, "synthetic/cumulative-decoded-bytes", decoded_bytes=65),
        _fixture("decompression-ratio", "check-decoded-stream-size", "decompression-ratio", "decoded-streams", 4, 20, "synthetic/decompression-ratio", compressed_bytes=4, decoded_bytes=20),
        _fixture("object-depth", "enter-depth", "object-depth", "document-model", 2, 3, "synthetic/object-depth", depth=3),
        _fixture("recursive-content-depth", "enter-depth", "recursive-content-depth", "document-model", 2, 3, "synthetic/recursive-content-depth", depth=3),
        _fixture("objects-visited", "charge-objects", "objects-visited", "document-model", 3, 4, "synthetic/objects-visited", objects=4),
        _fixture("render-operations", "charge-render-operations", "render-operations", "raster-tile", 3, 4, "synthetic/render-operations", operations=4),
        _fixture("render-pixels", "charge-render-pixels", "render-pixels", "raster-tile", 64, 65, "synthetic/render-pixels", pixels=65),
        _fixture("elapsed-time", "check-elapsed", "elapsed-time", "document-model", 1, 2, "synthetic/elapsed-time", elapsed_ms=2),
        _fixture("evidence-records", "charge-evidence-records", "evidence-records", "evidence-cache", 3, 4, "synthetic/evidence-records", records=4),
        _fixture("undo-snapshots", "charge-undo-snapshots", "undo-snapshots", "undo", 3, 4, "synthetic/undo-snapshots", snapshots=4),
        _fixture("rollback-artifacts", "charge-rollback-artifacts", "rollback-artifacts", "rollback", 3, 4, "synthetic/rollback-artifacts", artifacts=4),
    ]


def _canonical(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def manifest() -> dict[str, object]:
    records = fixtures()
    return {
        "schema_kind": SCHEMA_KIND,
        "schema_version": SCHEMA_VERSION,
        "generated_by": "scripts/budget_exhaustion/generate_corpus.py",
        "fixture_count": len(records),
        "fixtures": records,
    }


def expected_files() -> dict[str, bytes]:
    records = fixtures()
    files = {"manifest.json": _canonical(manifest())}
    for record in records:
        files[f"{record['id']}.json"] = _canonical(record)
    return files


def generate(output: Path, check: bool) -> int:
    expected = expected_files()
    problems: list[str] = []
    for name, content in expected.items():
        path = output / name
        if check:
            if not path.is_file():
                problems.append(f"missing {path}")
            elif path.read_bytes() != content:
                problems.append(f"stale generated fixture {path}")
        else:
            output.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    if check:
        actual = {path.name for path in output.glob("*.json")} if output.is_dir() else set()
        unexpected = sorted(actual - expected.keys())
        problems.extend(f"unexpected fixture {output / name}" for name in unexpected)
        if problems:
            for problem in problems:
                print(f"ERROR: {problem}", file=sys.stderr)
            return 1
        print(f"Budget exhaustion corpus is up to date ({len(fixtures())} fixtures).")
    else:
        print(f"Generated budget exhaustion corpus ({len(fixtures())} fixtures) in {output}.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true", help="verify committed output without writing")
    args = parser.parse_args()
    return generate(args.output, args.check)


if __name__ == "__main__":
    raise SystemExit(main())
