import json
import tempfile
import unittest
import zlib
from pathlib import Path

from scripts.resource_envelope.div2k_workload import (
    Example,
    _png_pixels,
    build_pdf,
    canonical_json,
    create_manifest,
    select_examples,
)


def _png(width: int, height: int, pixels: bytes) -> bytes:
    import struct

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", 0)

    rows = b"".join(b"\x00" + pixels[row * width * 3 : (row + 1) * width * 3] for row in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def _gray_png(width: int, height: int, pixels: bytes) -> bytes:
    import struct

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", 0)

    rows = b"".join(b"\x00" + pixels[row * width : (row + 1) * width] for row in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


class Div2kWorkloadTest(unittest.TestCase):
    def _corpus(self, root: Path) -> None:
        root.mkdir(parents=True)
        summary = {
            "dataset": "DIV2K",
            "source_counts": {"train": 1, "valid": 1},
            "examples_per_source": 2,
            "total_sources": 2,
            "total_examples": 4,
            "crop_size": [512, 512],
            "presets": ["all", "corner"],
            "widths_px": [8, 16],
        }
        (root / "dataset_summary.json").write_bytes(canonical_json(summary))
        for split, source in (("train", "0001"), ("valid", "0002")):
            for sample_index, (preset, width) in enumerate((("all", 8), ("corner", 16))):
                pair = root / split / source / f"sample_{sample_index:04d}"
                pair.mkdir(parents=True)
                pixels = bytes((sample_index * 10 + 10, 20, 30)) * 4
                pair.joinpath("input.png").write_bytes(_png(2, 2, pixels))
                pair.joinpath("ground_truth.png").write_bytes(_png(2, 2, pixels))
                pair.joinpath("mask.png").write_bytes(_gray_png(2, 2, bytes((255, 255, 255)) * 4))
                metadata = {
                    "source": f"{source}.png",
                    "source_split": split,
                    "sample_index": sample_index,
                    "preset": preset,
                    "width_px": width,
                    "crop_size": [512, 512],
                    "input_size": [2, 2],
                    "ground_truth_size": [2, 2],
                    "crop_box": [0, 0, 512, 512],
                    "seed": sample_index,
                }
                pair.joinpath("metadata.json").write_bytes(canonical_json(metadata))

    def test_png_decoder_reconstructs_rgb_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.png"
            pixels = bytes((1, 2, 3, 4, 5, 6))
            path.write_bytes(_png(2, 1, pixels))
            self.assertEqual(_png_pixels(path), (2, 1, pixels))

    def test_selection_is_deterministic_and_stratified(self):
        examples = [
            Example("train", f"{index:04d}", 0, Path(str(index)), {"preset": "all", "width_px": 8})
            for index in range(10)
        ] + [
            Example("valid", f"{index:04d}", 0, Path(str(index)), {"preset": "corner", "width_px": 16})
            for index in range(10)
        ]
        first = select_examples(examples, 6, 7)
        second = select_examples(examples, 6, 7)
        self.assertEqual([item.example_id for item in first], [item.example_id for item in second])
        self.assertEqual({item.stratum for item in first}, {("train", "all", 8), ("valid", "corner", 16)})

    def test_manifest_and_pdf_are_reproducible(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "corpus"
            self._corpus(root)
            manifest_path = Path(directory) / "manifest.json"
            manifest = create_manifest(root, manifest_path, 4, 0, True)
            self.assertEqual(manifest["selection"]["actual_count"], 4)
            first_pdf = Path(directory) / "first.pdf"
            second_pdf = Path(directory) / "second.pdf"
            first_summary = build_pdf(manifest_path, first_pdf, 8, 3)
            second_summary = build_pdf(manifest_path, second_pdf, 8, 3)
            self.assertEqual(first_summary["output_sha256"], second_summary["output_sha256"])
            self.assertEqual(first_pdf.read_bytes(), second_pdf.read_bytes())
            self.assertIn(b"/Count 8", first_pdf.read_bytes())


if __name__ == "__main__":
    unittest.main()
