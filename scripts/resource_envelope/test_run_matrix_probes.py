from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.resource_envelope.run_matrix import _failure_detail, matrix_failure_reasons, matrix_passes, run_cancellation_probe, run_fixture, run_hostile_corpus
from scripts.resource_envelope.validate_envelope import POOL_NAMES

CANDIDATE = "candidate-sha"


def _policy() -> dict:
    return {
        "resource_budget": {"resident_limit_bytes": 200, "pool_limits_bytes": {pool: 100 for pool in POOL_NAMES}},
        "workloads": {
            "pathological-vector": {"page_count": 256, "wall_time_ms": 100, "rss_high_water_bytes": 200, "cancellation_latency_ms": 50, "recovery_ms": 60000},
            "synthetic-image-heavy": {"page_count": 256, "wall_time_ms": 100, "rss_high_water_bytes": 200},
        },
    }


def _envelope(digest: str, status: str = "complete", preflight: int = 5, rss: int = 10, materialized: int = 256, latency: int = -1) -> dict:
    return {
        "identity": {"commit": CANDIDATE, "fixture_digest": digest},
        "family": "benchmark-render",
        "status": status,
        "page_count": 256,
        "rss_high_water_bytes": rss,
        "preflight_high_water_bytes": preflight,
        "pages_materialized": materialized,
        "elapsed_ms": 10,
        "cancellation_latency_ms": latency,
        "prefetch_shed": False,
        "interaction_slot_held": False,
        "resources": {
            "config": {"resident_limit_bytes": 200, "pool_limits_bytes": {pool: 100 for pool in POOL_NAMES}},
            "resident_bytes": 0,
            "resident_high_water_bytes": 0,
            "pressure": "normal",
            "pools": {pool: {"limit_bytes": 100, "current_bytes": 0, "high_water_bytes": 0, "evictions": 0, "shed": 0} for pool in POOL_NAMES},
        },
    }


def _process(command: list[str], returncode: int, envelope: dict | None) -> subprocess.CompletedProcess[str]:
    stdout = json.dumps({"data": {"workload_envelope": envelope}}) if envelope else ""
    return subprocess.CompletedProcess(command, returncode, stdout, "")


class _Fixture:
    def __enter__(self) -> "_Fixture":
        self._directory = tempfile.TemporaryDirectory()
        self.path = Path(self._directory.name) / "fixture.pdf"
        self.path.write_bytes(b"fixture")
        self.digest = hashlib.sha256(b"fixture").hexdigest()
        self.metadata = {"path": str(self.path), "sha256": self.digest, "size_bytes": 7, "provenance": "unit-test", "page_count": 256}
        return self

    def __exit__(self, *exc: object) -> None:
        self._directory.cleanup()

    def measure(self, runner, **kwargs) -> dict:
        return run_fixture(Path("PdfTool"), "pathological-vector", self.path, _policy(), 1, runner=runner, metadata=self.metadata, candidate_sha=CANDIDATE, **kwargs)


class MeasuredRunTest(unittest.TestCase):
    def test_complete_envelope_with_preflight_is_measured(self) -> None:
        with _Fixture() as fixture:
            record = fixture.measure(lambda command, **_: _process(command, 0, _envelope(fixture.digest)))
            self.assertEqual(record["status"], "measured", record["validation_errors"])
            self.assertGreaterEqual(record["runs"][0]["process_wall_ms"], 0)

    def test_preflight_profile_reaches_the_command(self) -> None:
        with _Fixture() as fixture:
            record = fixture.measure(lambda command, **_: _process(command, 0, _envelope(fixture.digest)), preflight_profile=Path("profile.json"))
            self.assertEqual(record["command"][-2:], ["--profile", "profile.json"])

    def test_crashed_process_fails_even_with_an_envelope(self) -> None:
        with _Fixture() as fixture:
            record = fixture.measure(lambda command, **_: _process(command, -11, _envelope(fixture.digest)))
            self.assertEqual(record["status"], "failed")
            self.assertIn("run 1: process-crashed:-11", record["validation_errors"])

    def test_timeout_fails(self) -> None:
        def runner(command, **kwargs):
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])

        with _Fixture() as fixture:
            record = fixture.measure(runner)
            self.assertEqual(record["status"], "failed")
            self.assertIn("run 1: benchmark-timeout", record["validation_errors"])

    def test_partial_output_exit_is_flagged_not_measured(self) -> None:
        with _Fixture() as fixture:
            record = fixture.measure(lambda command, **_: _process(command, 5, _envelope(fixture.digest)))
            self.assertEqual(record["status"], "flagged")

    def test_manifest_workload_overrides_the_default(self) -> None:
        with _Fixture() as fixture:
            fixture.metadata["workload"] = "synthetic-image-heavy"
            record = fixture.measure(lambda command, **_: _process(command, 0, _envelope(fixture.digest)))
            self.assertEqual(record["workload"], "synthetic-image-heavy")
            self.assertEqual(record["status"], "measured", record["validation_errors"])


class CancellationProbeTest(unittest.TestCase):
    def _probe(self, fixture: _Fixture, cancel_runner, runner) -> dict:
        return run_cancellation_probe(Path("PdfTool"), "pathological-vector", fixture.path, _policy(), 5, 0.5, CANDIDATE,
                                      "pathological-vector", cancel_runner=cancel_runner, runner=runner)

    def test_cancelled_run_and_reopen_are_measured(self) -> None:
        with _Fixture() as fixture:
            probe = self._probe(
                fixture,
                lambda command, timeout, cancel_after: _process(command, 6, _envelope(fixture.digest, status="cancelled", latency=20, materialized=40)),
                lambda command, **_: _process(command, 0, _envelope(fixture.digest, status="incomplete", preflight=-1, materialized=1)),
            )
            self.assertEqual(probe["status"], "measured", probe["validation_errors"])
            self.assertEqual(probe["cancellation"]["cancellation_latency_ms"], 20)
            self.assertGreaterEqual(probe["recovery"]["recovery_ms"], 0)
            self.assertEqual(probe["recovery"]["command"][-4:], ["--page-first", "1", "--page-last", "1"])

    def test_run_that_finished_before_the_interrupt_fails(self) -> None:
        with _Fixture() as fixture:
            probe = self._probe(
                fixture,
                lambda command, timeout, cancel_after: _process(command, 0, _envelope(fixture.digest)),
                lambda command, **_: _process(command, 0, _envelope(fixture.digest, status="incomplete", preflight=-1, materialized=1)),
            )
            self.assertEqual(probe["status"], "failed")
            self.assertTrue(any("not cancelled" in error or "ended 'complete'" in error for error in probe["validation_errors"]))

    def test_latency_over_policy_fails(self) -> None:
        with _Fixture() as fixture:
            probe = self._probe(
                fixture,
                lambda command, timeout, cancel_after: _process(command, 6, _envelope(fixture.digest, status="cancelled", latency=51)),
                lambda command, **_: _process(command, 0, _envelope(fixture.digest, status="incomplete", preflight=-1, materialized=1)),
            )
            self.assertIn("cancellation_latency_ms 51 exceeds workload policy 50", probe["validation_errors"])

    def test_hung_interrupt_fails(self) -> None:
        def cancel_runner(command, timeout, cancel_after):
            raise subprocess.TimeoutExpired(command, timeout)

        with _Fixture() as fixture:
            probe = self._probe(fixture, cancel_runner, lambda command, **_: _process(command, 0, _envelope(fixture.digest, status="incomplete", preflight=-1, materialized=1)))
            self.assertEqual(probe["status"], "failed")


class HostileCorpusTest(unittest.TestCase):
    def test_rejection_is_contained_and_crash_is_not(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            corpus = Path(directory)
            cases = []
            for case_id in ("rejected", "crashing"):
                (corpus / f"{case_id}.pdf").write_bytes(case_id.encode())
                cases.append({"id": case_id, "pdf": f"{case_id}.pdf", "sha256": hashlib.sha256(case_id.encode()).hexdigest()})
            (corpus / "manifest.json").write_text(json.dumps({"cases": cases}), encoding="utf-8")

            def runner(command, **_):
                return _process(command, 3 if "rejected.pdf" in command[2] else -6, None)

            hostile = run_hostile_corpus(Path("PdfTool"), corpus, _policy(), 5, runner=runner)
            by_id = {case["case_id"]: case for case in hostile["cases"]}
            self.assertEqual(by_id["rejected"]["status"], "contained")
            self.assertEqual(by_id["rejected"]["disposition"], "rejected")
            self.assertEqual(by_id["crashing"]["status"], "failed")
            self.assertEqual(hostile["summary"], {"total": 2, "contained": 1})

    def test_tampered_fixture_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            corpus = Path(directory)
            (corpus / "case.pdf").write_bytes(b"tampered")
            (corpus / "manifest.json").write_text(json.dumps({"cases": [{"id": "case", "pdf": "case.pdf", "sha256": "0" * 64}]}), encoding="utf-8")
            hostile = run_hostile_corpus(Path("PdfTool"), corpus, _policy(), 5, runner=lambda command, **_: _process(command, 0, None))
            self.assertEqual(hostile["cases"][0]["status"], "failed")


class MatrixPassTest(unittest.TestCase):
    def _matrix(self, probe_status: str | None = "measured", contained: int = 2, flagged: int = 0) -> dict:
        return {
            "summary": {"failed": 0, "flagged": flagged, "candidate_sha_verified": True},
            "cancellation_recovery_probe": {"status": probe_status} if probe_status else None,
            "hostile": {"summary": {"total": 2, "contained": contained}},
        }

    def test_strict_requires_probe_hostile_and_no_flags(self) -> None:
        self.assertTrue(matrix_passes(self._matrix(), strict=True))
        self.assertFalse(matrix_passes(self._matrix(probe_status=None), strict=True))
        self.assertFalse(matrix_passes(self._matrix(flagged=1), strict=True))
        self.assertTrue(matrix_passes(self._matrix(flagged=1), strict=False))

    def test_uncontained_hostile_case_fails_even_without_strict(self) -> None:
        self.assertFalse(matrix_passes(self._matrix(contained=1), strict=False))


class FailureDetailTest(unittest.TestCase):
    def test_keeps_render_errors_and_stderr_tail_of_a_partial_run(self) -> None:
        stdout = json.dumps({"data": {"nested": [{"rendering-errors": [{"page-no": 3, "message": "bad image"}]}]}})
        completed = subprocess.CompletedProcess(["PdfTool"], 5, stdout, "warning: x\n")
        detail = _failure_detail(completed)
        self.assertIn('"bad image"', detail)
        self.assertIn("stderr=warning: x", detail)

    def test_is_empty_when_nothing_was_reported(self) -> None:
        self.assertEqual(_failure_detail(subprocess.CompletedProcess(["PdfTool"], 5, "", "")), "")


class FailureReasonTest(unittest.TestCase):
    def test_names_every_failing_record_probe_and_hostile_case(self) -> None:
        matrix = {
            "fixtures": [
                {"fixture_id": "office-2mb", "status": "measured", "required": True, "validation_errors": []},
                {"fixture_id": "image-heavy-500mb", "status": "flagged", "required": True, "validation_errors": ["run 1: process exit code 1 is not success"],
                 "runs": [{"run": 1, "detail": "stderr=render failed"}]},
                {"fixture_id": "ten-thousand-page", "status": "unavailable", "required": True, "validation_errors": [], "reason": "fixture-not-found"},
                {"fixture_id": "multi-gb", "status": "unavailable", "required": False, "validation_errors": [], "reason": "fixture-not-supplied-optional"},
            ],
            "cancellation_recovery_probe": {"status": "failed", "validation_errors": ["recovery probe process-crashed:-11"]},
            "hostile": {"cases": [
                {"case_id": "ok", "status": "contained", "validation_errors": []},
                {"case_id": "deep-tree", "status": "failed", "process_exit_code": 0, "validation_errors": ["RSS 9 exceeds resident policy 4"]},
            ]},
        }
        self.assertEqual(matrix_failure_reasons(matrix), [
            "fixture image-heavy-500mb flagged: run 1: process exit code 1 is not success",
            "fixture image-heavy-500mb run 1 detail: stderr=render failed",
            "fixture ten-thousand-page unavailable: fixture-not-found",
            "probe failed: recovery probe process-crashed:-11",
            "hostile deep-tree (exit 0): RSS 9 exceeds resident policy 4",
        ])


if __name__ == "__main__":
    unittest.main()
