from __future__ import annotations

import hashlib
import re
import tempfile
import unittest
from pathlib import Path

from scripts.resource_envelope.run_matrix import FIXTURE_SPECS
from scripts.resource_envelope.synthetic_workload import ImageSpec, _PdfWriter, build_image_pages, build_office


def _xref_problems(pdf: bytes) -> list[str]:
    start = int(re.search(rb"startxref\n(\d+)\n%%EOF\n$", pdf).group(1))
    count = int(re.match(rb"xref\n0 (\d+)\n", pdf[start:]).group(1))
    rows = pdf[start:].split(b"\n")[2 : 2 + count]
    problems = [f"object {number}" for number, row in enumerate(rows[1:], start=1) if not pdf[int(row[:10]) :].startswith(f"{number} 0 obj".encode())]
    for match in re.finditer(rb"/Length (\d+) >>\nstream\n", pdf):
        if pdf[match.end() + int(match.group(1)) :][:10] != b"\nendstream":
            problems.append(f"stream at {match.start()}")
    return problems


class SyntheticWorkloadTest(unittest.TestCase):
    def test_image_pages_are_deterministic_and_well_formed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.pdf"
            second = Path(directory) / "second.pdf"
            build_image_pages(first, "ten-thousand-page", 30, 4, ImageSpec(16, 12))
            build_image_pages(second, "ten-thousand-page", 30, 4, ImageSpec(16, 12))
            pdf = first.read_bytes()
            self.assertEqual(hashlib.sha256(pdf).digest(), hashlib.sha256(second.read_bytes()).digest())
            self.assertEqual(pdf.count(b"/Type /Page "), 30)
            self.assertEqual(pdf.count(b"/Subtype /Image"), 4)
            self.assertIn(b"/Count 30", pdf)
            self.assertEqual(_xref_problems(pdf), [])

    def test_fixture_identity_changes_the_pixels(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            one = Path(directory) / "one.pdf"
            two = Path(directory) / "two.pdf"
            build_image_pages(one, "ten-thousand-page", 2, 1, ImageSpec(8, 8))
            build_image_pages(two, "image-heavy-500mb", 2, 1, ImageSpec(8, 8))
            self.assertNotEqual(one.read_bytes(), two.read_bytes())

    def test_office_fixture_fits_its_size_bounds(self) -> None:
        spec = FIXTURE_SPECS["office-2mb"]
        with tempfile.TemporaryDirectory() as directory:
            office = Path(directory) / "office.pdf"
            build_office(office)
            pdf = office.read_bytes()
            self.assertGreaterEqual(len(pdf), spec["min_bytes"])
            self.assertLessEqual(len(pdf), spec["max_bytes"])
            self.assertIn(b"/BaseFont /Helvetica", pdf)
            self.assertEqual(_xref_problems(pdf), [])

    def test_writer_rejects_unwritten_objects(self) -> None:
        with tempfile.TemporaryFile() as handle:
            writer = _PdfWriter(handle, 2)
            writer.add(1, b"<< >>")
            with self.assertRaises(ValueError):
                writer.finish("0" * 32)


if __name__ == "__main__":
    unittest.main()
