"""Deterministic image-heavy PDF workload builder for resource envelope qualification."""

from __future__ import annotations

import json
import struct
import zlib
from pathlib import Path

from scripts.resource_envelope.corpus import (
    SCHEMA_KIND,
    SCHEMA_VERSION,
    CorpusError,
    sha256_bytes,
    sha256_file,
)


def png_pixels(path: Path) -> tuple[int, int, bytes]:
    """Decode the corpus' non-interlaced 8-bit RGB/gray PNGs."""

    chunks: list[bytes] = []
    with path.open("rb") as handle:
        if handle.read(8) != b"\x89PNG\r\n\x1a\n":
            raise CorpusError(f"not a PNG: {path}")
        width = height = bit_depth = color_type = interlace = None
        while True:
            raw_length = handle.read(4)
            if not raw_length:
                break
            if len(raw_length) != 4:
                raise CorpusError(f"truncated PNG: {path}")
            length = struct.unpack(">I", raw_length)[0]
            kind = handle.read(4)
            data = handle.read(length)
            handle.read(4)
            if kind == b"IHDR":
                width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                    ">IIBBBBB", data
                )
            elif kind == b"IDAT":
                chunks.append(data)
            elif kind == b"IEND":
                break
    if (width, height, bit_depth, interlace) != (width, height, 8, 0):
        raise CorpusError(f"unsupported PNG encoding: {path}")
    if color_type not in (0, 2):
        raise CorpusError(f"unsupported PNG color type: {path}")
    channels = 1 if color_type == 0 else 3
    row_size = width * channels
    decoded = zlib.decompress(b"".join(chunks))
    expected_size = height * (row_size + 1)
    if len(decoded) != expected_size:
        raise CorpusError(f"unexpected PNG data size: {path}")
    rows: list[bytearray] = []
    offset = 0
    previous = bytearray(row_size)
    for _ in range(height):
        filter_type = decoded[offset]
        offset += 1
        encoded = decoded[offset : offset + row_size]
        offset += row_size
        row = bytearray(encoded)
        for index in range(row_size):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            up_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                row[index] = (row[index] + left) & 0xFF
            elif filter_type == 2:
                row[index] = (row[index] + up) & 0xFF
            elif filter_type == 3:
                row[index] = (row[index] + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:
                prediction = left + up - up_left
                distances = (abs(prediction - left), abs(prediction - up), abs(prediction - up_left))
                paeth = (left, up, up_left)[distances.index(min(distances))]
                row[index] = (row[index] + paeth) & 0xFF
            elif filter_type != 0:
                raise CorpusError(f"unsupported PNG filter {filter_type}: {path}")
        rows.append(row)
        previous = row
    if channels == 3:
        return width, height, b"".join(rows)
    rgb = bytearray(width * height * 3)
    for index, value in enumerate(b"".join(rows)):
        rgb[index * 3 : index * 3 + 3] = bytes((value, value, value))
    return width, height, bytes(rgb)


def _pdf_object(data: bytes, object_number: int) -> bytes:
    return f"{object_number} 0 obj\n".encode() + data + b"\nendobj\n"


def _pdf_stream(dictionary: str, stream: bytes) -> bytes:
    return (dictionary + f" /Length {len(stream)} >>\nstream\n").encode() + stream + b"\nendstream"


def build_pdf(manifest_path: Path, output: Path, page_count: int, page_seed: int) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_kind") != SCHEMA_KIND or manifest.get("schema_version") != SCHEMA_VERSION:
        raise CorpusError(f"unsupported workload manifest: {manifest_path}")
    examples = manifest.get("examples") or []
    if not examples:
        raise CorpusError("workload manifest has no examples")
    if page_count <= 0:
        raise ValueError("page count must be positive")
    corpus_root = Path(manifest["corpus"]["root"])
    selected_paths = [corpus_root / item["pair_dir"] / "input.png" for item in examples]
    for path in selected_paths:
        if not path.is_file():
            raise CorpusError(f"manifest input is missing: {path}")

    images: list[tuple[int, int, bytes]] = []
    image_index: dict[Path, int] = {}
    for path in selected_paths:
        if path not in image_index:
            image_index[path] = len(images)
            images.append(png_pixels(path))

    object_data: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R /Info 3 0 R >>",
        3: b"<< /Producer (Loop resource envelope) /CreationDate (D:20260101000000Z) >>",
    }
    image_object_numbers = []
    next_object = 4
    for width, height, pixels in images:
        image_object_numbers.append(next_object)
        compressed = zlib.compress(pixels, level=9)
        object_data[next_object] = _pdf_stream(
            f"<< /Type /XObject /Subtype /Image /Width {width} /Height {height}"
            " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode",
            compressed,
        )
        next_object += 1

    page_object_numbers = []
    for page_index in range(page_count):
        image_slot = (page_seed + page_index) % len(selected_paths)
        image_object = image_object_numbers[image_index[selected_paths[image_slot]]]
        image_width, image_height, _ = images[image_index[selected_paths[image_slot]]]
        page_object = next_object
        content_object = next_object + 1
        next_object += 2
        page_object_numbers.append(page_object)
        page_width, page_height = 612, 792
        scale = min(page_width / image_width, page_height / image_height)
        draw_width = image_width * scale
        draw_height = image_height * scale
        x = (page_width - draw_width) / 2
        y = (page_height - draw_height) / 2
        content = f"q {draw_width:.6f} 0 0 {draw_height:.6f} {x:.6f} {y:.6f} cm /Im0 Do Q".encode()
        object_data[page_object] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {page_width} {page_height}]"
            f" /Resources << /XObject << /Im0 {image_object} 0 R >> >>"
            f" /Contents {content_object} 0 R >>"
        ).encode()
        object_data[content_object] = _pdf_stream("<<", content)

    kids = " ".join(f"{number} 0 R" for number in page_object_numbers)
    object_data[2] = f"<< /Type /Pages /Kids [{kids}] /Count {page_count} >>".encode()
    object_count = max(object_data)
    manifest_digest = sha256_file(manifest_path)
    pdf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0] * (object_count + 1)
    for number in range(1, object_count + 1):
        offsets[number] = len(pdf)
        pdf.extend(_pdf_object(object_data[number], number))
    xref_offset = len(pdf)
    pdf.extend(f"xref\n0 {object_count + 1}\n".encode())
    pdf.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        pdf.extend(f"{offset:010d} 00000 n \n".encode())
    pdf.extend(
        f"trailer\n<< /Size {object_count + 1} /Root 1 0 R /Info 3 0 R "
        f"/ID [<{manifest_digest}> <{manifest_digest}>] >>\n"
        f"startxref\n{xref_offset}\n%%EOF\n".encode()
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(pdf)
    return {
        "schema_kind": SCHEMA_KIND,
        "schema_version": SCHEMA_VERSION,
        "workload": "div2k-image-heavy",
        "manifest": str(manifest_path.resolve()),
        "manifest_sha256": manifest_digest,
        "output": str(output.resolve()),
        "output_sha256": sha256_bytes(bytes(pdf)),
        "page_count": page_count,
        "unique_images": len(images),
        "page_seed": page_seed,
        "page_size_points": [612, 792],
    }
