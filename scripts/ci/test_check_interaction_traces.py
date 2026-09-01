#!/usr/bin/env python3
"""Tests for the interaction trace corpus and report checker."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from scripts.ci.check_interaction_traces import (  # noqa: E402
    corpus_digest,
    load_manifest,
    trend_rows,
    validate_corpus,
    validate_percentiles,
    validate_report,
    validate_scenario,
)


def minimal_scenario(**over) -> dict:
    scenario = {
        "schema_kind": "loupe-interaction-scenario",
        "schema_version": 1,
        "scenario_id": "example",
        "description": "An example scenario.",
        "fixture": {
            "page_count": 1,
            "page_size_mm": {"width": 210.0, "height": 297.0},
            "pixel_per_mm": 2.0,
            "device_pixel_ratio": 1.0,
            "initial_zoom": 1.0,
            "viewport_size_px": {"width": 800, "height": 600},
            "page_layout": "single-page",
        },
        "cost_model": {"base_frame_ns": 500000},
        "budgets": {
            "refresh_rate_hz": 60.0,
            "frame_p95_ms": 16.667,
            "input_to_frame_p95_ms": 16.667,
        },
        "expected": {"selected_id": ""},
        "input_script": [{"kind": "pointer-move", "at_px": {"x": 1, "y": 1}}],
    }
    scenario.update(over)
    return scenario


def duration(available=True, p50=1.0, p95=2.0, p99=3.0, sample_count=10) -> dict:
    if not available:
        return {"available": False, "sample_count": 0, "p50_ms": None, "p95_ms": None, "p99_ms": None}
    return {
        "available": True,
        "sample_count": sample_count,
        "p50_ms": p50,
        "p95_ms": p95,
        "p99_ms": p99,
    }


def minimal_report(**over) -> dict:
    report = {
        "schema_kind": "loupe-interaction-trace-report",
        "schema_version": 1,
        "lane": "deterministic",
        "identity": {
            "commit": "abc123",
            "compiler": "GNU 13.2",
            "os": "Linux",
            "qt": "6.11.1",
            "cpu": "x86_64",
            "renderer": "software",
            "fixture_digest": "deadbeef",
            "profile_or_operation_version": "interaction-trace-summary/2",
            "corpus_digest": "0" * 64,
        },
        "runs": [
            {
                "scenario_id": "example",
                "status": "verified",
                "trace_id": "example",
                "summary_schema_version": 2,
                "budgets": {},
                "samples": {},
                "input_to_frame_ms": duration(),
                "frame_time_ms": duration(),
                "stage_ms": {},
                "slow_frame_causes": {},
                "hit_test": {},
                "async_overlap": {},
                "page_surface_cache": {"hits": 1, "misses": 0},
                "present_timing": {
                    "available": False,
                    "reason": "interaction-trace/present-timing-unavailable",
                },
                "passed": True,
                "first_violated_contract": None,
                "responsible_phase": None,
                "failure_excerpt": [],
            }
        ],
    }
    report.update(over)
    return report


class CorpusTests(unittest.TestCase):
    def test_repository_corpus_passes(self):
        self.assertEqual(validate_corpus(), [])

    def test_every_manifest_scenario_exists_and_matches(self):
        manifest = load_manifest()
        self.assertTrue(manifest["scenarios"])
        for entry in manifest["scenarios"]:
            self.assertEqual(entry["issue"], 146)

    def test_corpus_digest_is_order_independent(self):
        manifest = load_manifest()
        reversed_manifest = {
            **manifest,
            "scenarios": list(reversed(manifest["scenarios"])),
        }
        self.assertEqual(corpus_digest(manifest), corpus_digest(reversed_manifest))

    def test_detects_digest_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            corpus = root / "UnitTests" / "testdata" / "interaction-traces"
            corpus.mkdir(parents=True)
            (corpus / "example.json").write_text(
                json.dumps(minimal_scenario()), encoding="utf-8"
            )
            (corpus / "manifest.json").write_text(
                json.dumps(
                    {
                        "schema_kind": "loupe-interaction-corpus",
                        "schema_version": 1,
                        "scenarios": [
                            {
                                "id": "example",
                                "path": "UnitTests/testdata/interaction-traces/example.json",
                                "issue": 146,
                                "sha256": "f" * 64,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            violations = validate_corpus(corpus, root)
            self.assertTrue(any("sha256 mismatch" in reason for _, reason in violations))

    def test_digest_survives_a_crlf_checkout(self):
        """.gitattributes checks text out as CRLF; the digests must not care.

        Without this the corpus gate passes on the machine that wrote the
        manifest and fails on every fresh CI checkout.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            corpus = root / "UnitTests" / "testdata" / "interaction-traces"
            corpus.mkdir(parents=True)

            source = pathlib.Path(__file__).resolve().parents[2] / "UnitTests" / "testdata" / "interaction-traces"
            for path in source.glob("*.json"):
                (corpus / path.name).write_bytes(
                    path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
                )

            self.assertEqual(validate_corpus(corpus, root), [])

    def test_untracked_scenario_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            corpus = root / "UnitTests" / "testdata" / "interaction-traces"
            corpus.mkdir(parents=True)
            (corpus / "stray.json").write_text(json.dumps(minimal_scenario()), encoding="utf-8")
            (corpus / "manifest.json").write_text(
                json.dumps(
                    {
                        "schema_kind": "loupe-interaction-corpus",
                        "schema_version": 1,
                        "scenarios": [
                            {
                                "id": "example",
                                "path": "UnitTests/testdata/interaction-traces/example.json",
                                "issue": 146,
                                "sha256": "f" * 64,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            violations = validate_corpus(corpus, root)
            self.assertTrue(any("missing from manifest" in reason for _, reason in violations))


class ScenarioTests(unittest.TestCase):
    def test_minimal_scenario_passes(self):
        self.assertEqual(validate_scenario(minimal_scenario(), "example"), [])

    def test_rejects_both_trace_and_script(self):
        scenario = minimal_scenario(trace={"schema_version": 1, "inputs": [{}]})
        violations = validate_scenario(scenario, "example")
        self.assertTrue(any("exactly one of" in reason for _, reason in violations))

    def test_rejects_neither_trace_nor_script(self):
        scenario = minimal_scenario()
        del scenario["input_script"]
        violations = validate_scenario(scenario, "example")
        self.assertTrue(any("exactly one of" in reason for _, reason in violations))

    def test_rejects_duplicate_hit_target_ids(self):
        scenario = minimal_scenario()
        bounds = {"x": 0.0, "y": 0.0, "width": 1.0, "height": 1.0}
        scenario["fixture"]["hit_targets"] = [
            {"kind": "finding", "page_index": 0, "id": "f-1", "page_bounds": bounds},
            {"kind": "finding", "page_index": 0, "id": "f-1", "page_bounds": bounds},
        ]
        violations = validate_scenario(scenario, "example")
        self.assertTrue(any("duplicate hit target" in reason for _, reason in violations))

    def test_rejects_non_kebab_scenario_id(self):
        violations = validate_scenario(minimal_scenario(scenario_id="Example_One"), "example")
        self.assertTrue(any("kebab-case" in reason for _, reason in violations))

    def test_rejects_fractional_cost(self):
        scenario = minimal_scenario()
        scenario["cost_model"]["hit_test_ns_per_candidate"] = 1.5
        violations = validate_scenario(scenario, "example")
        self.assertTrue(any("nanoseconds" in reason for _, reason in violations))


class PercentileTests(unittest.TestCase):
    """The no-zero-for-missing rule from docs/RESOURCE_BUDGETS.md."""

    def test_available_block_passes(self):
        self.assertEqual(validate_percentiles(duration(), "latency"), [])

    def test_unavailable_block_passes_with_nulls(self):
        self.assertEqual(validate_percentiles(duration(available=False), "latency"), [])

    def test_zero_standing_in_for_missing_is_rejected(self):
        block = {"available": False, "sample_count": 0, "p50_ms": 0.0, "p95_ms": 0.0, "p99_ms": 0.0}
        violations = validate_percentiles(block, "latency")
        self.assertTrue(any("must be null" in reason for _, reason in violations))

    def test_available_block_with_nulls_is_rejected(self):
        block = {"available": True, "sample_count": 4, "p50_ms": None, "p95_ms": None, "p99_ms": None}
        violations = validate_percentiles(block, "latency")
        self.assertTrue(any("must be a number" in reason for _, reason in violations))

    def test_unavailable_block_may_not_claim_samples(self):
        block = {"available": False, "sample_count": 7, "p50_ms": None, "p95_ms": None, "p99_ms": None}
        violations = validate_percentiles(block, "latency")
        self.assertTrue(any("sample_count must be 0" in reason for _, reason in violations))


class ReportTests(unittest.TestCase):
    def test_minimal_report_passes(self):
        self.assertEqual(validate_report(minimal_report()), [])

    def test_failed_run_must_name_contract_and_phase(self):
        report = minimal_report()
        report["runs"][0].update(passed=False, failure_excerpt=[])
        violations = validate_report(report)
        self.assertTrue(any("first_violated_contract" in reason for _, reason in violations))
        self.assertTrue(any("responsible_phase" in reason for _, reason in violations))
        self.assertTrue(any("failure_excerpt" in reason for _, reason in violations))

    def test_failed_run_with_attribution_passes(self):
        report = minimal_report()
        report["runs"][0].update(
            passed=False,
            first_violated_contract="p95-input-to-frame",
            responsible_phase="overlay",
            failure_excerpt=["p95 input-to-frame 24.10 ms exceeds 16.67 ms"],
        )
        self.assertEqual(validate_report(report), [])

    def test_rejects_unknown_contract(self):
        report = minimal_report()
        report["runs"][0].update(
            passed=False,
            first_violated_contract="vibes",
            responsible_phase="overlay",
            failure_excerpt=["something"],
        )
        violations = validate_report(report)
        self.assertTrue(any("first_violated_contract" in reason for _, reason in violations))

    def test_passing_run_may_not_name_a_violation(self):
        report = minimal_report()
        report["runs"][0]["first_violated_contract"] = "final-state"
        violations = validate_report(report)
        self.assertTrue(any("must not name a violated contract" in reason for _, reason in violations))

    def test_verified_status_requires_available_latency(self):
        report = minimal_report()
        report["runs"][0]["input_to_frame_ms"] = duration(available=False)
        violations = validate_report(report)
        self.assertTrue(any("verified but" in reason for _, reason in violations))

    def test_static_only_run_may_report_unavailable_latency(self):
        report = minimal_report()
        report["runs"][0]["status"] = "static-only"
        report["runs"][0]["input_to_frame_ms"] = duration(available=False)
        self.assertEqual(validate_report(report), [])

    def test_present_timing_unavailable_needs_a_reason(self):
        report = minimal_report()
        report["runs"][0]["present_timing"] = {"available": False, "reason": "  "}
        violations = validate_report(report)
        self.assertTrue(any("must carry a reason" in reason for _, reason in violations))

    def test_missing_corpus_scenario_is_reported(self):
        violations = validate_report(minimal_report(), corpus_ids={"example", "pan"})
        self.assertTrue(any("has no run" in reason for _, reason in violations))

    def test_unknown_run_is_reported(self):
        violations = validate_report(minimal_report(), corpus_ids=set())
        self.assertTrue(any("is not in the corpus" in reason for _, reason in violations))

    def test_duplicate_run_is_reported(self):
        report = minimal_report()
        report["runs"].append(copy.deepcopy(report["runs"][0]))
        violations = validate_report(report)
        self.assertTrue(any("duplicate scenario_id" in reason for _, reason in violations))

    def test_corpus_digest_mismatch_is_reported(self):
        violations = validate_report(minimal_report(), expected_corpus_digest="a" * 64)
        self.assertTrue(any("does not match the tracked corpus" in reason for _, reason in violations))

    def test_empty_identity_field_is_reported(self):
        report = minimal_report()
        report["identity"]["commit"] = "  "
        violations = validate_report(report)
        self.assertTrue(any("commit must be a non-empty string" in reason for _, reason in violations))


class TrendTests(unittest.TestCase):
    def test_trend_reports_delta_against_baseline(self):
        report = minimal_report()
        baseline = minimal_report()
        baseline["runs"][0]["input_to_frame_ms"] = duration(p95=1.5)
        rows = trend_rows(report, baseline)
        self.assertTrue(any("p95 delta +0.500ms" in row for row in rows))

    def test_trend_says_unavailable_rather_than_zero(self):
        report = minimal_report()
        report["runs"][0]["status"] = "static-only"
        report["runs"][0]["input_to_frame_ms"] = duration(available=False)
        rows = trend_rows(report, None)
        self.assertTrue(any("unavailable" in row for row in rows))


if __name__ == "__main__":
    unittest.main()
