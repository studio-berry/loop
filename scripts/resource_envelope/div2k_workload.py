#!/usr/bin/env python3
"""Validate the external DIV2K bleed corpus and build deterministic PDF workloads.

The corpus is intentionally external to the Loupe repository.  The default
location is the generated corpus used by the bleed research lab::

    C:\\.dev\\repos\\l-bleed\\results\\DIV2K

Examples::

    python scripts/resource_envelope/div2k_workload.py validate \
        --corpus-root C:\\.dev\\repos\\l-bleed\\results\\DIV2K
    python scripts/resource_envelope/div2k_workload.py manifest \
        --corpus-root C:\\.dev\\repos\\l-bleed\\results\\DIV2K \
        --output C:\\temp\\loupe-div2k-manifest.json --sample-count 256
    python scripts/resource_envelope/div2k_workload.py build-pdf \
        --manifest C:\\temp\\loupe-div2k-manifest.json \
        --output C:\\temp\\loupe-div2k-10000-pages.pdf --page-count 10000

The PDF writer deliberately supports the corpus' 8-bit RGB, non-interlaced PNG
files without requiring a package installation.  Workload outputs and their
manifests belong outside the repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence


DEFAULT_CORPUS_ROOT = Path(r"C:\.dev\repos\l-bleed\results\DIV2K")
SCHEMA_KIND = "loupe-resource-envelope"
SCHEMA_VERSION = 1
EXPECTED_SOURCE_COUNTS = {"train": 800, "valid": 100}
PAIR_FILES = ("input.png", "ground_truth.png", "mask.png", "metadata.json")


class CorpusError(ValueError):
    """Raised when the external corpus does not satisfy its file contract."""


@dataclass(frozen=True)
class Example:
    split: str
    source: str
    sample_index: int
    pair_dir: Path
    metadata: dict

    @property
    def example_id(self) -> str:
        return f"{self.split}/{self.source}/sample_{self.sample_index:04d}"

    @property
    def stratum(self) -> tuple[str, str, int]:
        return (
            self.split,
            str(self.metadata.get("preset", "unknown")),
            int(self.metadata.get("width_px", -1)),
        )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


def read_png_header(path: Path) -> tuple[int, int, int, int, int]:
    """Return width, height, bit depth, color type, and interlace method."""

    with path.open("rb") as handle:
        if handle.read(8) != b"\x89PNG\r\n\x1a\n":
            raise CorpusError(f"not a PNG: {path}")
        length_bytes = handle.read(4)
        chunk_type = handle.read(4)
        if len(length_bytes) != 4 or chunk_type != b"IHDR":
            raise CorpusError(f"missing PNG IHDR: {path}")
        length = struct.unpack(">I", length_bytes)[0]
        header = handle.read(length)
        if len(header) != 13:
            raise CorpusError(f"truncated PNG IHDR: {path}")
        width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
            ">IIBBBBB", header
        )
        return width, height, bit_depth, color_type, interlace


def _load_summary(corpus_root: Path) -> tuple[Path, dict]:
    summary_path = corpus_root / "dataset_summary.json"
    if not summary_path.is_file():
        raise CorpusError(f"missing corpus summary: {summary_path}")
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise CorpusError(f"invalid corpus summary: {summary_path}: {exc}") from exc
    if summary.get("dataset") != "DIV2K":
        raise CorpusError(f"unexpected dataset name in {summary_path}")
    return summary_path, summary


def _iter_pair_dirs_exact(corpus_root: Path) -> Iterator[Path]:
    for split in ("train", "valid"):
        split_root = corpus_root / split
        if not split_root.is_dir():
            raise CorpusError(f"missing split directory: {split_root}")
        for source_root in sorted(path for path in split_root.iterdir() if path.is_dir()):
            for pair_dir in sorted(path for path in source_root.iterdir() if path.is_dir()):
                yield pair_dir


def _read_example(pair_dir: Path, corpus_root: Path) -> Example:
    try:
        relative_parts = pair_dir.relative_to(corpus_root).parts
    except ValueError as exc:
        raise CorpusError(f"pair is outside corpus root: {pair_dir}") from exc
    if len(relative_parts) != 3 or relative_parts[0] not in EXPECTED_SOURCE_COUNTS:
        raise CorpusError(f"unexpected pair directory layout: {pair_dir}")
    split, source, sample_name = relative_parts
    if not sample_name.startswith("sample_"):
        raise CorpusError(f"unexpected sample directory: {pair_dir}")
    try:
        sample_index = int(sample_name.removeprefix("sample_"))
    except ValueError as exc:
        raise CorpusError(f"invalid sample directory: {pair_dir}") from exc

    metadata_path = pair_dir / "metadata.json"
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CorpusError(f"invalid metadata: {metadata_path}: {exc}") from exc
    if metadata.get("sample_index") != sample_index:
        raise CorpusError(f"sample index mismatch: {metadata_path}")
    if metadata.get("source_split") != split:
        raise CorpusError(f"source split mismatch: {metadata_path}")
    if metadata.get("source") != f"{source}.png":
        raise CorpusError(f"source mismatch: {metadata_path}")
    if not isinstance(metadata.get("crop_size"), list) or metadata["crop_size"] != [512, 512]:
        raise CorpusError(f"unexpected crop size: {metadata_path}")

    for filename in PAIR_FILES:
        path = pair_dir / filename
        if not path.is_file():
            raise CorpusError(f"missing pair file: {path}")

    expected_input = tuple(metadata.get("input_size", ()))
    expected_ground_truth = tuple(metadata.get("ground_truth_size", ()))
    input_header = read_png_header(pair_dir / "input.png")
    ground_truth_header = read_png_header(pair_dir / "ground_truth.png")
    mask_header = read_png_header(pair_dir / "mask.png")
    for name, header in (
        ("input.png", input_header),
        ("ground_truth.png", ground_truth_header),
        ("mask.png", mask_header),
    ):
        width, height, bit_depth, color_type, interlace = header
        if bit_depth != 8 or interlace != 0:
            raise CorpusError(f"unsupported PNG encoding in {pair_dir / name}")
        if color_type not in (0, 2):
            raise CorpusError(f"unsupported PNG color type in {pair_dir / name}")
        if name == "input.png" and (width, height) != expected_input:
            raise CorpusError(f"input dimensions mismatch: {pair_dir}")
        if name == "ground_truth.png" and (width, height) != expected_ground_truth:
            raise CorpusError(f"ground-truth dimensions mismatch: {pair_dir}")
    if mask_header[:2] != ground_truth_header[:2] or mask_header[3] != 0:
        raise CorpusError(f"mask dimensions/color mismatch: {pair_dir}")

    return Example(split, source, sample_index, pair_dir, metadata)


def load_examples(corpus_root: Path, *, hash_all: bool = False) -> tuple[dict, list[Example], str]:
    corpus_root = corpus_root.resolve()
    summary_path, summary = _load_summary(corpus_root)
    examples = [_read_example(pair_dir, corpus_root) for pair_dir in _iter_pair_dirs_exact(corpus_root)]
    summary_source_counts = summary.get("source_counts")
    if not isinstance(summary_source_counts, dict):
        raise CorpusError("corpus summary has no source_counts object")
    expected_source_counts = {
        split: int(summary_source_counts.get(split, -1)) for split in EXPECTED_SOURCE_COUNTS
    }
    expected_total = sum(expected_source_counts.values()) * int(summary.get("examples_per_source", 0))
    if len(examples) != expected_total or len(examples) != int(summary.get("total_examples", -1)):
        raise CorpusError(
            f"example count mismatch: expected {expected_total}, found {len(examples)}"
        )
    for split, expected_count in expected_source_counts.items():
        split_examples = [example for example in examples if example.split == split]
        source_count = len({example.source for example in split_examples})
        if source_count != expected_count:
            raise CorpusError(f"{split} source count mismatch: {source_count}")

    records = []
    for example in examples:
        record = {
            "id": example.example_id,
            "pair_dir": example.pair_dir.relative_to(corpus_root).as_posix(),
            "files": {filename: (example.pair_dir / filename).stat().st_size for filename in PAIR_FILES},
        }
        if hash_all:
            record["sha256"] = {
                filename: sha256_file(example.pair_dir / filename) for filename in PAIR_FILES
            }
        records.append(record)
    digest_payload = {
        "summary_sha256": sha256_file(summary_path),
        "records": records,
    }
    return summary, examples, sha256_bytes(canonical_json(digest_payload))


def _selection_key(seed: int, example: Example) -> str:
    return hashlib.sha256(f"{seed}\0{example.example_id}".encode("utf-8")).hexdigest()


def select_examples(examples: Sequence[Example], count: int, seed: int) -> list[Example]:
    if count <= 0:
        raise ValueError("sample count must be positive")
    if count > len(examples):
        raise ValueError(f"sample count {count} exceeds corpus size {len(examples)}")
    groups: dict[tuple[str, str, int], list[Example]] = {}
    for example in examples:
        groups.setdefault(example.stratum, []).append(example)
    for group in groups.values():
        group.sort(key=lambda item: _selection_key(seed, item))
    strata = sorted(groups)
    selected: list[Example] = []
    cursor = 0
    while len(selected) < count:
        stratum = strata[cursor % len(strata)]
        group = groups[stratum]
        if group:
            selected.append(group.pop())
        cursor += 1
        if cursor > count * len(strata) + len(examples):
            raise RuntimeError("selection could not satisfy requested count")
    return sorted(selected, key=lambda item: item.example_id)


def _example_record(example: Example, corpus_root: Path, include_hashes: bool) -> dict:
    record = {
        "id": example.example_id,
        "split": example.split,
        "source": example.source,
        "sample_index": example.sample_index,
        "pair_dir": example.pair_dir.relative_to(corpus_root).as_posix(),
        "preset": example.metadata.get("preset"),
        "width_px": example.metadata.get("width_px"),
        "crop_box": example.metadata.get("crop_box"),
        "seed": example.metadata.get("seed"),
        "files": {filename: (example.pair_dir / filename).stat().st_size for filename in PAIR_FILES},
    }
    if include_hashes:
        record["sha256"] = {
            filename: sha256_file(example.pair_dir / filename) for filename in PAIR_FILES
        }
    return record


def create_manifest(
    corpus_root: Path, output: Path, sample_count: int, seed: int, hash_all: bool
) -> dict:
    summary, examples, corpus_digest = load_examples(corpus_root, hash_all=hash_all)
    selected = select_examples(examples, sample_count, seed)
    corpus_root = corpus_root.resolve()
    manifest = {
        "schema_kind": SCHEMA_KIND,
        "schema_version": SCHEMA_VERSION,
        "corpus": {
            "name": "DIV2K",
            "root": str(corpus_root),
            "summary_sha256": sha256_file(corpus_root / "dataset_summary.json"),
            "corpus_digest": corpus_digest,
            "total_examples": len(examples),
            "source_counts": summary.get("source_counts"),
            "examples_per_source": summary.get("examples_per_source"),
            "crop_size": summary.get("crop_size"),
            "presets": summary.get("presets"),
            "widths_px": summary.get("widths_px"),
        },
        "selection": {
            "seed": seed,
            "requested_count": sample_count,
            "actual_count": len(selected),
            "hash_all": hash_all,
            "ordering": "example-id",
        },
        "examples": [_example_record(example, corpus_root, hash_all) for example in selected],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(canonical_json(manifest))
    return manifest


def _png_pixels(path: Path) -> tuple[int, int, bytes]:
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
            handle.read(4)  # CRC; the file contract is validated separately.
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
            images.append(_png_pixels(path))

    object_data: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R /Info 3 0 R >>",
        3: b"<< /Producer (Loupe resource envelope) /CreationDate (D:20260101000000Z) >>",
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


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate the external DIV2K corpus")
    validate.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    validate.add_argument("--hash-all", action="store_true", help="hash every pair file")

    manifest = subparsers.add_parser("manifest", help="write a deterministic selection manifest")
    manifest.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    manifest.add_argument("--output", type=Path, required=True)
    manifest.add_argument("--sample-count", type=int, default=256)
    manifest.add_argument("--seed", type=int, default=0)
    manifest.add_argument("--hash-all", action="store_true")

    pdf = subparsers.add_parser("build-pdf", help="build an external deterministic image-heavy PDF")
    pdf.add_argument("--manifest", type=Path, required=True)
    pdf.add_argument("--output", type=Path, required=True)
    pdf.add_argument("--page-count", type=int, default=10000)
    pdf.add_argument("--page-seed", type=int, default=0)
    pdf.add_argument("--summary-output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "validate":
            summary, examples, digest = load_examples(args.corpus_root, hash_all=args.hash_all)
            print(json.dumps({
                "dataset": summary["dataset"],
                "examples": len(examples),
                "sources": summary.get("total_sources"),
                "corpus_digest": digest,
                "hash_all": args.hash_all,
            }, indent=2))
        elif args.command == "manifest":
            manifest = create_manifest(
                args.corpus_root, args.output, args.sample_count, args.seed, args.hash_all
            )
            print(json.dumps({
                "output": str(args.output.resolve()),
                "examples": manifest["selection"]["actual_count"],
                "corpus_digest": manifest["corpus"]["corpus_digest"],
            }, indent=2))
        else:
            summary = build_pdf(args.manifest, args.output, args.page_count, args.page_seed)
            if args.summary_output:
                args.summary_output.parent.mkdir(parents=True, exist_ok=True)
                args.summary_output.write_bytes(canonical_json(summary))
            print(json.dumps(summary, indent=2))
    except (CorpusError, OSError, ValueError, zlib.error) as exc:
        print(f"resource-envelope error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
