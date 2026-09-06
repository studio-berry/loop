#!/usr/bin/env python3
"""Generate the deterministic synthetic processing-budget exhaustion corpus."""

from __future__ import annotations

import argparse
import json
import sys
import zlib
from pathlib import Path


DEFAULT_OUTPUT = Path(__file__).resolve().parents[2] / "UnitTests" / "testdata" / "budget-exhaustion"
SCHEMA_KIND = "loop-processing-budget-exhaustion-corpus"
SCHEMA_VERSION = 2


def _fixture(
    fixture_id: str,
    operation: str,
    kind: str,
    pool: str,
    limit: int,
    attempted: int,
    context: str,
    pdf_shape: str = "basic",
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
        "pdf_file": f"{fixture_id}.pdf",
        "pdf_shape": pdf_shape,
        "payload": payload,
    }


def fixtures() -> list[dict[str, object]]:
    """Return all budget dimensions in stable order.

    The payloads are deliberately small. Each record also names a deterministic
    minimal PDF whose structure exercises the production reader's corresponding
    parser or stream path without checking in third-party or large binary input.
    """

    return [
        _fixture("input-bytes", "charge-input-bytes", "input-bytes", "document-model", 64, 65, "synthetic/input-bytes", source_bytes=65),
        _fixture("single-decoded-stream-bytes", "check-decoded-stream-size", "single-decoded-stream-bytes", "decoded-streams", 64, 65, "synthetic/single-decoded-stream-bytes", pdf_shape="flate-stream", compressed_bytes=65, decoded_bytes=65),
        _fixture("cumulative-decoded-bytes", "charge-decoded-bytes", "cumulative-decoded-bytes", "decoded-streams", 64, 65, "synthetic/cumulative-decoded-bytes", pdf_shape="many-flate-streams", decoded_bytes=65),
        _fixture("decompression-ratio", "check-decoded-stream-size", "decompression-ratio", "decoded-streams", 4, 5, "synthetic/decompression-ratio", pdf_shape="flate-stream", compressed_bytes=4, decoded_bytes=20),
        _fixture("object-depth", "enter-depth", "object-depth", "document-model", 2, 3, "synthetic/object-depth", pdf_shape="nested-arrays", depth=3),
        _fixture("recursive-content-depth", "enter-depth", "recursive-content-depth", "document-model", 2, 3, "synthetic/recursive-content-depth", pdf_shape="recursive-form", depth=3),
        _fixture("objects-visited", "charge-objects", "objects-visited", "document-model", 3, 4, "synthetic/objects-visited", pdf_shape="many-objects", objects=4),
        _fixture("render-operations", "charge-render-operations", "render-operations", "raster-tile", 3, 4, "synthetic/render-operations", pdf_shape="many-drawing-operations", operations=4),
        _fixture("render-pixels", "charge-render-pixels", "render-pixels", "raster-tile", 64, 65, "synthetic/render-pixels", pdf_shape="huge-image", pixels=65),
        _fixture("elapsed-time", "check-elapsed", "elapsed-time", "document-model", 1, 2, "synthetic/elapsed-time", elapsed_ms=2),
        _fixture("evidence-records", "charge-evidence-records", "evidence-records", "evidence-cache", 3, 4, "synthetic/evidence-records", records=4),
        _fixture("undo-snapshots", "charge-undo-snapshots", "undo-snapshots", "undo", 3, 4, "synthetic/undo-snapshots", snapshots=4),
        _fixture("rollback-artifacts", "charge-rollback-artifacts", "rollback-artifacts", "rollback", 3, 4, "synthetic/rollback-artifacts", artifacts=4),
    ]


def _canonical(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _normalize_newlines(content: bytes) -> bytes:
    """Normalize checkout-specific CRLF endings for bytewise verification."""

    return content.replace(b"\r\n", b"\n")


def _pdf(objects: list[bytes]) -> bytes:
    """Serialize a small classic-xref PDF with deterministic byte offsets."""

    header = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    body = bytearray(header)
    offsets = [0]
    for number, value in enumerate(objects, start=1):
        offsets.append(len(body))
        body.extend(f"{number} 0 obj\n".encode("ascii"))
        body.extend(value)
        body.extend(b"\nendobj\n")
    xref_offset = len(body)
    body.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    body.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        body.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    body.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
        f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii")
    )
    return bytes(body)


def _flate_stream(decoded: bytes) -> bytes:
    compressed = zlib.compress(decoded, level=9)
    return b"<< /Length %d /Filter /FlateDecode >>\nstream\n%s\nendstream" % (len(compressed), compressed)


def _pdf_for_shape(shape: str) -> bytes:
    page_content = b"BT /F1 12 Tf 10 100 Td (Budget corpus) Tj ET"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length %d >>\nstream\n%s\nendstream" % (len(page_content), page_content),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    if shape == "flate-stream":
        decoded = b"A" * 128
        objects.append(_flate_stream(decoded))
    elif shape == "many-flate-streams":
        for _ in range(8):
            objects.append(_flate_stream(b"B" * 16))
    elif shape == "nested-arrays":
        objects.append(b"[ [ [ [ [0] ] ] ] ]")
    elif shape == "recursive-form":
        form = b"/F1 Do"
        objects[3] = b"<< /Length %d /Resources << /XObject << /F1 6 0 R >> >> >>\nstream\n%s\nendstream" % (len(form), form)
        objects.append(b"<< /Type /XObject /Subtype /Form /BBox [0 0 200 200] /Resources << /XObject << /F1 6 0 R >> >> /Length 6 >>\nstream\n/F1 Do\nendstream")
    elif shape == "many-objects":
        for index in range(20):
            objects.append(f"<< /CorpusObject {index} >>".encode("ascii"))
    elif shape == "many-drawing-operations":
        operations = b"".join(b"0 0 m 100 100 l S\n" for _ in range(128))
        objects[3] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(operations), operations)
    elif shape == "huge-image":
        objects.append(b"<< /Type /XObject /Subtype /Image /Width 100000 /Height 100000 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 0 >>\nstream\n\nendstream")
    return _pdf(objects)


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
        files[str(record["pdf_file"])] = _pdf_for_shape(str(record["pdf_shape"]))
    return files


def generate(output: Path, check: bool) -> int:
    expected = expected_files()
    problems: list[str] = []
    for name, content in expected.items():
        path = output / name
        if check:
            if not path.is_file():
                problems.append(f"missing {path}")
            elif _normalize_newlines(path.read_bytes()) != content:
                problems.append(f"stale generated fixture {path}")
        else:
            output.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    if check:
        actual = {path.name for path in output.glob("*.json")} if output.is_dir() else set()
        if output.is_dir():
            actual.update(path.name for path in output.glob("*.pdf"))
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
