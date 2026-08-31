from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.resource_envelope.pathological_workload import build_pathological_pdf


class PathologicalWorkloadTest(unittest.TestCase):
    def test_vector_workload_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.pdf"
            second = Path(directory) / "second.pdf"
            first_summary = build_pathological_pdf(first, 8, 32, "pathological-vector")
            second_summary = build_pathological_pdf(second, 8, 32, "pathological-vector")
            self.assertEqual(first_summary, second_summary)
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_transparency_workload_has_distinct_family(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            summary = build_pathological_pdf(Path(directory) / "spots.pdf", 3, 12, "transparency-spots")
            self.assertEqual(summary["family"], "transparency-spots")
            self.assertEqual(summary["page_count"], 3)
            self.assertGreater(summary["bytes"], 0)


if __name__ == "__main__":
    unittest.main()
