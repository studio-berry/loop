#!/usr/bin/env python3
"""Create small deterministic vector/transparency stress PDFs for CI."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Literal


Family = Literal["pathological-vector", "transparency-spots"]


def _stream_object(payload: bytes) -> bytes:
    return b"<< /Length " + str(len(payload)).encode("ascii") + b" >>\nstream\n" + payload + b"\nendstream"


def _content(family: Family, operations: int, page_index: int) -> bytes:
    lines = [b"q", b"0.95 0.95 0.95 rg", b"0 0 612 792 re f", b"Q"]
    for operation in range(operations):
        x = 8 + (operation * 17 + page_index * 3) % 580
        y = 8 + (operation * 29 + page_index * 5) % 760
        width = 4 + operation % 23
        height = 3 + (operation * 7) % 19
        red = (operation % 5) / 5.0
        green = ((operation + page_index) % 7) / 7.0
        blue = ((operation * 3 + page_index) % 11) / 11.0
        lines.extend([
            f"{red:.6f} {green:.6f} {blue:.6f} rg".encode("ascii"),
            f"{x} {y} {width} {height} re f".encode("ascii"),
        ])
    if family == "transparency-spots":
        lines.extend([b"q", b"/GS1 gs", b"/CS1 cs", b"0.5 scn", b"100 120 412 210 re f", b"Q"])
    return b"\n".join(lines) + b"\n"


def build_pathological_pdf(output: Path, page_count: int, operations: int, family: Family) -> dict[str, object]:
    if page_count < 1 or operations < 1:
        raise ValueError("page_count and operations must be positive")

    object_bodies: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"",  # Filled after page references are known.
    ]
    function_reference = None
    if family == "transparency-spots":
        object_bodies.append(b"<< /Type /ExtGState /ca 0.45 /CA 0.45 >>")
        function_reference = len(object_bodies) + 1
        object_bodies.append(b"<< /FunctionType 2 /Domain [0 1] /Range [0 1 0 1 0 1] /C0 [0 0 0] /C1 [1 0 0] /N 1 >>")

    page_references: list[int] = []
    for page_index in range(page_count):
        contents_reference = len(object_bodies) + 2
        page_reference = len(object_bodies) + 1
        resources = b"<< /ProcSet [/PDF] /ColorSpace << /CS1 [/Separation /Spot1 /DeviceRGB " + str(function_reference).encode("ascii") + b" 0 R] >> /ExtGState << /GS1 3 0 R >> >>" if family == "transparency-spots" else b"<< /ProcSet [/PDF] >>"
        group = b" /Group << /S /Transparency /CS /DeviceRGB >>" if family == "transparency-spots" else b""
        page = b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources " + resources + group + b" /Contents " + str(contents_reference).encode("ascii") + b" 0 R >>"
        object_bodies.append(page)
        object_bodies.append(_stream_object(_content(family, operations, page_index)))
        page_references.append(page_reference)

    kids = b" ".join(str(reference).encode("ascii") + b" 0 R" for reference in page_references)
    object_bodies[1] = b"<< /Type /Pages /Count " + str(page_count).encode("ascii") + b" /Kids [" + kids + b"] >>"

    output.parent.mkdir(parents=True, exist_ok=True)
    pdf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, body in enumerate(object_bodies, start=1):
        offsets.append(len(pdf))
        pdf.extend(f"{number} 0 obj\n".encode("ascii"))
        pdf.extend(body)
        pdf.extend(b"\nendobj\n")
    xref = len(pdf)
    pdf.extend(f"xref\n0 {len(object_bodies) + 1}\n".encode("ascii"))
    pdf.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        pdf.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf.extend(f"trailer\n<< /Size {len(object_bodies) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
    output.write_bytes(pdf)
    return {
        "family": family,
        "page_count": page_count,
        "operations_per_page": operations,
        "bytes": len(pdf),
        "sha256": hashlib.sha256(pdf).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--page-count", type=int, default=256)
    parser.add_argument("--operations", type=int, default=512)
    parser.add_argument("--family", choices=("pathological-vector", "transparency-spots"), default="pathological-vector")
    parser.add_argument("--summary-output", type=Path)
    args = parser.parse_args()
    summary = build_pathological_pdf(args.output, args.page_count, args.operations, args.family)
    if args.summary_output:
        args.summary_output.parent.mkdir(parents=True, exist_ok=True)
        args.summary_output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
