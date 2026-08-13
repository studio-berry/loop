"""Contract tests for OCR request normalization and bbox mapping."""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "service"))

from engine import normalize_languages, pixel_bbox_to_pdf, validate_request  # noqa: E402


class EngineContractTest(unittest.TestCase):
    def test_languages_are_normalized_and_deduplicated(self) -> None:
        self.assertEqual(normalize_languages([" EN ", "fr", "en", "FR"]), ["en", "fr"])
        self.assertEqual(normalize_languages(None), ["en"])

    def test_invalid_language_shape_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "languages must be an array"):
            normalize_languages("en")

    def test_request_limits_and_media_box_are_validated(self) -> None:
        normalized = validate_request(
            {
                "page": 1,
                "dpi": 300,
                "image": "page.png",
                "languages": ["EN"],
                "media_box": {"x": 10, "y": 20, "width": 600, "height": 800},
                "rotation": 90,
            }
        )
        self.assertEqual(normalized["languages"], ["en"])
        self.assertEqual(normalized["rotation"], 90)

        with self.assertRaisesRegex(ValueError, "dpi"):
            validate_request({"page": 1, "dpi": 1201, "image": "page.png"})

    def test_bbox_top_left_and_nonzero_origin(self) -> None:
        result = pixel_bbox_to_pdf(
            [[0, 0], [100, 0], [100, 100], [0, 100]],
            1000,
            1000,
            {"x": 10, "y": 20, "width": 600, "height": 800},
        )
        self.assertEqual(result, {"x": 10.0, "y": 740.0, "width": 60.0, "height": 80.0})

    def test_bbox_rotation_90(self) -> None:
        result = pixel_bbox_to_pdf(
            [[0, 0], [100, 0], [100, 100], [0, 100]],
            1000,
            800,
            {"x": 0, "y": 0, "width": 600, "height": 800},
            90,
        )
        self.assertEqual(result, {"x": 0.0, "y": 0.0, "width": 75.0, "height": 80.0})

    def test_invalid_bbox_never_emits_non_finite_values(self) -> None:
        result = pixel_bbox_to_pdf(
            [[math.nan, 0], [math.inf, 1]],
            1000,
            1000,
            {"x": 0, "y": 0, "width": 600, "height": 800},
        )
        self.assertTrue(all(math.isfinite(value) for value in result.values()))
        self.assertEqual(result["width"], 0.0)


if __name__ == "__main__":
    unittest.main()
