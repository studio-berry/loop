from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from PIL import Image

from scripts.qualification.compare_independent_renders import measure


class IndependentRenderTest(unittest.TestCase):
    def test_fixed_region_and_tolerance_expose_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            core, oracle, delta = root / "core.png", root / "oracle.tif", root / "delta.png"
            first = Image.new("L", (8, 8), 255)
            first.save(core)
            fixture = {"width": 8, "height": 8, "regions": [[1, 1, 6, 6]], "max_channel_delta": 2, "differing_pixel_budget": 0}
            second = first.copy()
            second.putpixel((0, 0), 0)
            second.putpixel((3, 3), 253)
            second.save(oracle)
            self.assertEqual(measure(core, oracle, fixture, delta)["status"], "passed")
            second.putpixel((3, 3), 252)
            second.save(oracle)
            result = measure(core, oracle, fixture, delta)
            self.assertEqual(result["status"], "rejected")
            self.assertEqual(result["regions"][0]["differing_pixels"], 1)
            self.assertTrue(delta.is_file())
            self.assertEqual(fixture["differing_pixel_budget"], 0)

    def test_wrong_geometry_and_outside_regions_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            core, oracle, delta = root / "core.png", root / "oracle.tif", root / "delta.png"
            Image.new("L", (8, 8), 255).save(core)
            Image.new("L", (7, 8), 255).save(oracle)
            fixture = {"width": 8, "height": 8, "regions": [[1, 1, 6, 6]], "max_channel_delta": 2, "differing_pixel_budget": 0}
            with self.assertRaisesRegex(ValueError, "geometry"):
                measure(core, oracle, fixture, delta)
            Image.new("L", (8, 8), 255).save(oracle)
            fixture["regions"] = [[1, 1, 8, 8]]
            with self.assertRaisesRegex(ValueError, "region"):
                measure(core, oracle, fixture, delta)


if __name__ == "__main__":
    unittest.main()
