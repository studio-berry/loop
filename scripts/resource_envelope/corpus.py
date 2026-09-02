"""DIV2K corpus validation, selection, and manifest generation."""

from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Sequence

DEFAULT_CORPUS_ROOT = Path(r"C:\.dev\repos\l-bleed\results\DIV2K")
SCHEMA_KIND = "loop-resource-envelope"
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
