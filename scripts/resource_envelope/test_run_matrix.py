from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.resource_envelope.run_matrix import FIXTURE_SPECS, _fixture_args, run_fixture, run_matrix
from scripts.resource_envelope.validate_envelope import POOL_NAMES


def _policy() -> dict:
    return {
        "resource_budget": {
            "resident_limit_bytes": 200,
            "pool_limits_bytes": {pool: 100 for pool in POOL_NAMES},
        },
        "workloads": {
            "pathological-vector": {"page_count": 256, "wall_time_ms": 100, "rss_high_water_bytes": 200},
        },
    }


def _envelope(page_count: int = 256, rss: int = 10, elapsed: int = 10) -> dict:
    return {
        "identity": {},
        "family": "test",
        "status": "incomplete",
        "page_count": page_count,
        "rss_high_water_bytes": rss,
        "preflight_high_water_bytes": -1,
        "pages_materialized": page_count,
        "elapsed_ms": elapsed,
        "prefetch_shed": False,
        "interaction_slot_held": True,
        "resources": {
            "config": {"resident_limit_bytes": 200, "pool_limits_bytes": {pool: 100 for pool in POOL_NAMES}},
            "resident_bytes": 0,
            "resident_high_water_bytes": 0,
            "pressure": "normal",
            "pools": {
                pool: {"limit_bytes": 100, "current_bytes": 0, "high_water_bytes": 0, "evictions": 0, "shed": 0}
                for pool in POOL_NAMES
            },
        },
    }


class RunMatrixTest(unittest.TestCase):
    def test_fixture_argument_rejects_unknown_name(self) -> None:
        with self.assertRaises(ValueError):
            _fixture_args(["unknown=file.pdf"])

    def test_run_fixture_extracts_nested_envelope_and_flags_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            payload = json.dumps({"data": {"workload_envelope": _envelope()}})

            def runner(*args, **kwargs):
                return subprocess.CompletedProcess(args[0], 0, payload, "")

            record = run_fixture(Path("PdfTool.exe"), "pathological-vector", fixture, _policy(), 1, runner=runner)
            self.assertEqual(record["status"], "flagged")
            self.assertEqual(record["result"]["page_count"], 256)

    def test_baseline_regression_fails_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            payload = json.dumps({"workload_envelope": _envelope(rss=50, elapsed=50)})

            def runner(*args, **kwargs):
                return subprocess.CompletedProcess(args[0], 0, payload, "")

            baseline = {"result": _envelope(rss=10, elapsed=10)}
            record = run_fixture(Path("PdfTool.exe"), "pathological-vector", fixture, _policy(), 1, baseline=baseline, margin=2, runner=runner)
            self.assertEqual(record["status"], "failed")
            self.assertEqual(len(record["regressions"]), 2)

    def test_matrix_records_missing_required_and_optional_fixtures(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            matrix = run_matrix(Path("PdfTool.exe"), {}, _policy(), 1)
            self.assertEqual(matrix["summary"]["total"], len(FIXTURE_SPECS))
            self.assertEqual(matrix["summary"]["flagged"], sum(spec["required"] for spec in FIXTURE_SPECS.values()))
            self.assertEqual(matrix["summary"]["failed"], 0)


if __name__ == "__main__":
    unittest.main()
