from __future__ import annotations

import hashlib
import re
import tempfile
import unittest
import zlib
from pathlib import Path

from scripts.resource_envelope.budget_exhaustion_corpus import (
    BUILDERS,
    assemble_pdf,
    generate_corpus,
)


class AssemblePdfTest(unittest.TestCase):
    def test_rejects_non_contiguous_object_numbers(self) -> None:
        with self.assertRaises(ValueError):
            assemble_pdf({1: b"<< >>", 3: b"<< >>"})

    def test_xref_offsets_point_at_the_right_object(self) -> None:
        pdf = assemble_pdf({1: b"<< /Type /Catalog >>", 2: b"42"})
        match = re.search(rb"startxref\r?\n(\d+)\r?\n%%EOF", pdf)
        assert match is not None
        xref_offset = int(match.group(1))
        self.assertEqual(pdf[xref_offset : xref_offset + 4], b"xref")

        header_match = re.match(rb"xref\r?\n0 (\d+)\r?\n", pdf[xref_offset:])
        assert header_match is not None
        count = int(header_match.group(1))
        self.assertEqual(count, 3)
        position = xref_offset + header_match.end()
        for number in range(count):
            entry = pdf[position : position + 20]
            position += 20
            if number == 0:
                continue
            offset = int(entry[:10])
            self.assertEqual(pdf[offset : offset + len(f"{number} 0 obj".encode())], f"{number} 0 obj".encode())


class BudgetExhaustionCorpusTest(unittest.TestCase):
    def test_generation_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first_manifest = generate_corpus(Path(first_dir))
            second_manifest = generate_corpus(Path(second_dir))
            self.assertEqual(first_manifest, second_manifest)
            for case in first_manifest["cases"]:
                first_bytes = (Path(first_dir) / case["pdf"]).read_bytes()
                second_bytes = (Path(second_dir) / case["pdf"]).read_bytes()
                self.assertEqual(first_bytes, second_bytes)

    def test_manifest_has_one_case_per_builder(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            self.assertEqual(len(manifest["cases"]), len(BUILDERS))

    def test_every_case_is_small_and_hash_matches_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            for case in manifest["cases"]:
                path = Path(directory) / case["pdf"]
                data = path.read_bytes()
                self.assertLess(len(data), 64 * 1024, f"{case['id']} fixture is unexpectedly large")
                self.assertEqual(hashlib.sha256(data).hexdigest(), case["sha256"])

    def test_every_case_has_required_manifest_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            seen_kinds = set()
            for case in manifest["cases"]:
                for field in ("id", "description", "path", "expected", "limits"):
                    self.assertIn(field, case)
                self.assertIn(case["path"], ("session", "reader"))
                self.assertIn("kind", case["expected"])
                self.assertIn("pool", case["expected"])
                if case["path"] == "session":
                    self.assertIn("profile", case)
                    self.assertIn("checks", case["profile"])
                seen_kinds.add(case["expected"]["kind"])

            required_kinds = {
                "decompression-ratio",
                "cumulative-decoded-bytes",
                "recursive-content-depth",
                "render-operations",
                "render-pixels",
                "object-depth",
                "objects-visited",
            }
            self.assertEqual(seen_kinds, required_kinds)

    def test_decompression_bomb_ratio_exceeds_its_own_tightened_limit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            case = next(item for item in manifest["cases"] if item["id"] == "decompression-bomb")
            data = (Path(directory) / case["pdf"]).read_bytes()
            stream_match = re.search(rb"stream\r?\n", data)
            assert stream_match is not None
            start = stream_match.end()
            end = data.index(b"\nendstream", start)
            compressed = data[start:end]
            decoded = zlib.decompress(compressed)
            ratio = len(decoded) / len(compressed)
            self.assertGreater(ratio, case["limits"]["maxDecompressionRatio"])

    def test_cumulative_decoded_bytes_case_exceeds_its_own_tightened_cap(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            case = next(item for item in manifest["cases"] if item["id"] == "cumulative-decoded-bytes")
            data = (Path(directory) / case["pdf"]).read_bytes()
            total_stream_bytes = sum(
                len(match.group(1)) for match in re.finditer(rb"stream\r?\n(.*?)\r?\nendstream", data, re.DOTALL)
            )
            self.assertGreater(total_stream_bytes, case["limits"]["maxCumulativeDecodedBytes"])

    def test_deep_recursive_object_graph_nests_past_its_own_tightened_depth(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            case = next(item for item in manifest["cases"] if item["id"] == "deep-recursive-object-graph")
            data = (Path(directory) / case["pdf"]).read_bytes()
            deepest_run = max(len(run) for run in re.findall(rb"\[+", data))
            self.assertGreater(deepest_run, case["limits"]["maxObjectDepth"])

    def test_pathological_object_count_exceeds_its_own_tightened_cap(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = generate_corpus(Path(directory))
            case = next(item for item in manifest["cases"] if item["id"] == "pathological-object-count")
            data = (Path(directory) / case["pdf"]).read_bytes()
            object_count = len(re.findall(rb"\d+ 0 obj", data))
            self.assertGreater(object_count, case["limits"]["maxObjectsVisited"])


if __name__ == "__main__":
    unittest.main()
