from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.resource_envelope.run_matrix import (
    FIXTURE_SPECS,
    _fixture_args,
    _load_fixture_manifest,
    run_fixture,
    run_matrix,
)
from scripts.resource_envelope.create_fixture_manifest import create_manifest
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


def _envelope(page_count: int = 256, rss: int = 10, elapsed: int = 10, commit: str | None = None, fixture_digest: str | None = None) -> dict:
    identity = {}
    if commit is not None or fixture_digest is not None:
        identity = {"commit": commit, "fixture_digest": fixture_digest}
    return {
        "identity": identity,
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


def _metadata(fixture: Path) -> dict:
    import hashlib

    return {
        "path": str(fixture),
        "sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "size_bytes": fixture.stat().st_size,
        "provenance": "unit-test fixture",
        "page_count": 256,
    }


class RunMatrixTest(unittest.TestCase):
    def test_fixture_argument_rejects_unknown_name(self) -> None:
        with self.assertRaises(ValueError):
            _fixture_args(["unknown=file.pdf"])

    def test_run_fixture_extracts_nested_envelope_and_flags_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            import hashlib
            digest = hashlib.sha256(b"fixture").hexdigest()
            payload = json.dumps({"data": {"workload_envelope": _envelope(commit="candidate-sha", fixture_digest=digest)}})

            def runner(*args, **kwargs):
                return subprocess.CompletedProcess(args[0], 0, payload, "")

            record = run_fixture(Path("PdfTool.exe"), "pathological-vector", fixture, _policy(), 1, runner=runner, metadata=_metadata(fixture), candidate_sha="candidate-sha")
            self.assertEqual(record["status"], "flagged")
            self.assertEqual(record["result"]["page_count"], 256)

    def test_baseline_regression_fails_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            import hashlib
            digest = hashlib.sha256(b"fixture").hexdigest()
            payload = json.dumps({"workload_envelope": _envelope(rss=50, elapsed=50, commit="candidate-sha", fixture_digest=digest)})

            def runner(*args, **kwargs):
                return subprocess.CompletedProcess(args[0], 0, payload, "")

            baseline = {"result": _envelope(rss=10, elapsed=10, commit="candidate-sha", fixture_digest=digest)}
            metadata = _metadata(fixture)
            current = run_fixture(Path("PdfTool.exe"), "pathological-vector", fixture, _policy(), 1, baseline=None, runner=runner, metadata=metadata, candidate_sha="candidate-sha")
            baseline["fixture_sha256"] = current["fixture_sha256"]
            baseline["identity"] = current["identity"]
            record = run_fixture(Path("PdfTool.exe"), "pathological-vector", fixture, _policy(), 1, baseline=baseline, margin=2, runner=runner, metadata=metadata, candidate_sha="candidate-sha")
            self.assertEqual(record["status"], "failed")
            self.assertEqual(len(record["regressions"]), 2)

    def test_identity_must_match_candidate_and_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            import hashlib
            payload = json.dumps(
                {
                    "workload_envelope": _envelope(
                        commit="stale-candidate",
                        fixture_digest=hashlib.sha256(b"other fixture").hexdigest(),
                    ),
                }
            )

            def runner(*args, **kwargs):
                return __import__("subprocess").CompletedProcess(args[0], 0, payload, "")

            metadata = _metadata(fixture)
            record = run_fixture(
                Path("PdfTool.exe"),
                "pathological-vector",
                fixture,
                _policy(),
                1,
                runner=runner,
                metadata=metadata,
                candidate_sha="candidate-sha",
            )

            self.assertEqual(record["status"], "failed")
            self.assertTrue(
                any("identity.commit" in error for error in record["validation_errors"])
            )
            self.assertTrue(
                any("identity.fixture_digest" in error for error in record["validation_errors"])
            )

    def test_repetitions_record_conservative_memory_and_median_time(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            import hashlib
            digest = hashlib.sha256(b"fixture").hexdigest()
            envelopes = [_envelope(rss=value, elapsed=value, commit="candidate-sha", fixture_digest=digest) for value in (10, 20, 30)]

            def runner(*args, **kwargs):
                return subprocess.CompletedProcess(args[0], 0, json.dumps({"workload_envelope": envelopes.pop(0)}), "")

            record = run_fixture(Path("PdfTool.exe"), "pathological-vector", fixture, _policy(), 1, runner=runner, metadata=_metadata(fixture), repetitions=3, candidate_sha="candidate-sha")
            self.assertEqual(record["statistics"]["elapsed_ms"]["median"], 20)
            self.assertEqual(record["result"]["rss_high_water_bytes"], 30)

    def test_manifest_resolves_relative_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "schema_kind": "loupe-resource-envelope-fixtures",
                "schema_version": 1,
                "fixtures": [{
                    "fixture_id": "pathological-vector",
                    "path": "fixture.pdf",
                    "sha256": "0" * 64,
                    "size_bytes": 7,
                    "provenance": "unit-test",
                }],
            }), encoding="utf-8")
            loaded = _load_fixture_manifest(manifest)
            self.assertEqual(Path(loaded["pathological-vector"]["path"]), fixture.resolve())

    def test_create_manifest_records_digest_size_and_expected_pages(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.pdf"
            fixture.write_bytes(b"fixture")
            manifest = create_manifest({"pathological-vector": fixture}, "unit-test generator")
            record = manifest["fixtures"][0]
            self.assertEqual(record["size_bytes"], 7)
            self.assertEqual(record["page_count"], 256)
            self.assertEqual(record["provenance"], "unit-test generator")

    def test_matrix_records_missing_required_and_optional_fixtures(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            matrix = run_matrix(Path("PdfTool.exe"), {}, _policy(), 1)
            self.assertEqual(matrix["summary"]["total"], len(FIXTURE_SPECS))
            self.assertEqual(matrix["summary"]["flagged"], sum(spec["required"] for spec in FIXTURE_SPECS.values()))
            self.assertEqual(matrix["summary"]["failed"], 0)


if __name__ == "__main__":
    unittest.main()
