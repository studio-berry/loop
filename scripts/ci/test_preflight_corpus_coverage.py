#!/usr/bin/env python3
"""Unit tests for the preflight corpus-coverage map generator.

Mirrors scripts/ci/test_preflight_check_catalog.py. The map is a generated
artifact that claims which golden-corpus fixtures exercise which catalog row, so
every rule that claim rests on is asserted to fail closed: a fixture without a
snapshot, a snapshot without a fixture, a snapshot naming an unregistered check,
a `covered` row with no exercising fixture, a row with no exercising fixture and
no hand-written reason, a stale reason on an exercised row, an overlay with no
row for a registered check, and a matrix id that disagrees with the engine's.
Two tests pin the other half of the claim: a finding that reports an inspection
failure, and a status that reports no inspection, must not read as exercising a
row.
"""

from __future__ import annotations

import contextlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

GENERATOR_PATH = ROOT / "scripts" / "generate-architecture-catalogs.py"
spec = importlib.util.spec_from_file_location("generate_architecture_catalogs", GENERATOR_PATH)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
OVERLAY_PATH = ROOT / "docs" / "preflight-check-catalog-overlay.json"
CORPUS_MAP_PATH = ROOT / "docs" / "generated" / "preflight-corpus-coverage.json"

REVIEWED_CORPUS_GAPS = []


def synthetic_manifest_and_snapshots(exclude: str | None = None) -> tuple[list[dict], dict[str, dict]]:
    """One fixture per registered check, each producing one finding for that check.

    ``exclude`` omits that check's fixture, which is how a corpus gap is
    simulated without touching the real corpus.
    """
    manifest: list[dict] = []
    snapshots: dict[str, dict] = {}
    for check_id in generator.parse_preflight_checks():
        if check_id == exclude:
            continue
        fixture = f"exercises-{check_id}"
        manifest.append(
            {
                "id": fixture,
                "pdf": f"{fixture}.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": False, "check_ids": [check_id]},
            }
        )
        snapshots[fixture] = {
            "schema_version": 4,
            "pass": False,
            "checks": [{"id": check_id, "status": "failed"}],
            "errors": [{"check_id": check_id, "type": check_id, "severity": "error"}],
            "warnings": [],
        }
    return manifest, snapshots


def synthetic_overlay(gaps: dict[str, str]) -> dict:
    """The real overlay with ``corpus_gap`` present on exactly the named rows."""
    overlay = json.loads(OVERLAY_PATH.read_text(encoding="utf-8"))
    for entry in overlay["checks"].values():
        entry.pop("corpus_gap", None)
    for check_id, reason in gaps.items():
        overlay["checks"][check_id]["corpus_gap"] = reason
    return overlay


@contextlib.contextmanager
def patched_corpus(manifest: list[dict], snapshots: dict[str, dict], overlay: dict):
    """Point the generator at a throwaway corpus and overlay."""
    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        snapshot_dir = root / "snapshots"
        snapshot_dir.mkdir()
        manifest_path = root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        for fixture, report in snapshots.items():
            (snapshot_dir / f"{fixture}.json").write_text(json.dumps(report), encoding="utf-8")
        overlay_path = root / "overlay.json"
        overlay_path.write_text(json.dumps(overlay), encoding="utf-8")
        original = (
            generator.CORPUS_MANIFEST_PATH,
            generator.CORPUS_SNAPSHOT_DIR,
            generator.PREFLIGHT_OVERLAY_PATH,
        )
        generator.CORPUS_MANIFEST_PATH = manifest_path
        generator.CORPUS_SNAPSHOT_DIR = snapshot_dir
        generator.PREFLIGHT_OVERLAY_PATH = overlay_path
        try:
            yield
        finally:
            (
                generator.CORPUS_MANIFEST_PATH,
                generator.CORPUS_SNAPSHOT_DIR,
                generator.PREFLIGHT_OVERLAY_PATH,
            ) = original


class PreflightCorpusCoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = generator.parse_preflight_checks()

    def map(self) -> dict:
        return generator.build_preflight_corpus_coverage(self.registry)

    def test_map_rows_cover_the_registry_both_ways(self) -> None:
        coverage = self.map()
        self.assertEqual(sorted(coverage["rows"]), sorted(self.registry))
        self.assertEqual(coverage["matrix_id"], generator.parse_engine_matrix_id())

    def test_generated_map_matches_committed_copy(self) -> None:
        expected = generator.serialized_preflight_corpus_coverage()
        self.assertEqual(CORPUS_MAP_PATH.read_text(encoding="utf-8"), expected)

    def test_covered_row_without_an_exercising_fixture_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="image-resolution")
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(
                ValueError,
                "covered check 'image-resolution' has no corpus fixture exercising it",
            ):
                self.map()

    def test_row_without_an_exercising_fixture_needs_a_recorded_reason(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="dieline")
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(
                ValueError,
                "check 'dieline' has no corpus fixture and no recorded corpus_gap reason",
            ):
                self.map()

    def test_reason_on_an_exercised_row_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        overlay = synthetic_overlay({"bleed": "stale reason"})
        with patched_corpus(manifest, snapshots, overlay):
            with self.assertRaisesRegex(
                ValueError,
                "check 'bleed' has corpus fixtures but a recorded corpus_gap reason",
            ):
                self.map()

    def test_blank_corpus_gap_on_an_exercised_row_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        overlay = synthetic_overlay({})
        overlay["checks"]["bleed"]["corpus_gap"] = ""
        with patched_corpus(manifest, snapshots, overlay):
            with self.assertRaisesRegex(
                ValueError,
                "check 'bleed' has corpus fixtures but a recorded corpus_gap reason",
            ):
                self.map()

    def test_malformed_corpus_gap_on_an_unexercised_row_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="dieline")
        overlay = synthetic_overlay({})
        overlay["checks"]["dieline"]["corpus_gap"] = 165
        with patched_corpus(manifest, snapshots, overlay):
            with self.assertRaisesRegex(ValueError, "check 'dieline' has a malformed corpus_gap reason"):
                self.map()

    def test_fixture_without_a_committed_snapshot_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        manifest.append(
            {
                "id": "ghost",
                "pdf": "ghost.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": True, "check_ids": []},
            }
        )
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(ValueError, "corpus fixture 'ghost' has no committed snapshot"):
                self.map()

    def test_snapshot_without_a_manifest_entry_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        snapshots["orphan"] = {
            "schema_version": 4,
            "pass": True,
            "checks": [],
            "errors": [],
            "warnings": [],
        }
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(ValueError, "snapshots with no corpus manifest entry: orphan"):
                self.map()

    def test_snapshot_naming_an_unregistered_check_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        first = sorted(snapshots)[0]
        snapshots[first]["errors"] = [{"check_id": "not-a-check", "type": "not-a-check"}]
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(ValueError, "reports unregistered check 'not-a-check'"):
                self.map()

    def test_the_reviewed_corpus_gaps_are_still_the_only_gaps(self) -> None:
        coverage = self.map()
        self.assertEqual(coverage["corpus_gaps"], REVIEWED_CORPUS_GAPS)

    def test_clean_pass_fixtures_do_not_exercise_a_row(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="trim")
        manifest.append(
            {
                "id": "clean",
                "pdf": "clean.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": True, "check_ids": []},
            }
        )
        snapshots["clean"] = {
            "schema_version": 4,
            "pass": True,
            "checks": [{"id": "trim", "status": "ok"}],
            "errors": [],
            "warnings": [],
        }
        with patched_corpus(manifest, snapshots, synthetic_overlay({"trim": "no fixture yet"})):
            with self.assertRaisesRegex(
                ValueError,
                "covered check 'trim' has no corpus fixture exercising it",
            ):
                self.map()

    def test_inspection_failure_findings_do_not_exercise_a_row(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="trim")
        manifest.append(
            {
                "id": "trim-mask-overflow",
                "pdf": "trim-mask-overflow.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": False, "check_ids": ["trim"]},
            }
        )
        snapshots["trim-mask-overflow"] = {
            "schema_version": 4,
            "pass": False,
            "checks": [{"id": "trim", "status": "incomplete"}],
            "errors": [],
            "warnings": [{"check_id": "trim", "type": "check-incomplete", "severity": "info"}],
        }
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(
                ValueError,
                "covered check 'trim' has no corpus fixture exercising it",
            ):
                self.map()

    def test_uninspected_status_does_not_exercise_a_row(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="trim")
        manifest.append(
            {
                "id": "trim-skipped",
                "pdf": "trim-skipped.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": False, "check_ids": []},
            }
        )
        snapshots["trim-skipped"] = {
            "schema_version": 4,
            "pass": False,
            "checks": [{"id": "trim", "status": "skipped"}],
            "errors": [],
            "warnings": [],
        }
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(
                ValueError,
                "covered check 'trim' has no corpus fixture exercising it",
            ):
                self.map()

    def test_substantive_finding_with_uninspected_status_does_not_exercise_a_row(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="trim")
        manifest.append(
            {
                "id": "trim-skipped-with-finding",
                "pdf": "trim-skipped-with-finding.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": False, "check_ids": ["trim"]},
            }
        )
        snapshots["trim-skipped-with-finding"] = {
            "schema_version": 4,
            "pass": False,
            "checks": [{"id": "trim", "status": "skipped"}],
            "errors": [{"check_id": "trim", "type": "trim", "severity": "error"}],
            "warnings": [],
        }
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            with self.assertRaisesRegex(
                ValueError,
                "covered check 'trim' has no corpus fixture exercising it",
            ):
                self.map()

    def test_aborted_check_findings_do_not_exercise_a_row(self) -> None:
        for finding_type in ("check-error", "budget-exceeded"):
            with self.subTest(finding_type=finding_type):
                manifest, snapshots = synthetic_manifest_and_snapshots(exclude="trim")
                manifest.append(
                    {
                        "id": f"trim-{finding_type}",
                        "pdf": f"trim-{finding_type}.pdf",
                        "profile": "profiles/loop-default.json",
                        "expect": {"pass": False, "check_ids": ["trim"]},
                    }
                )
                snapshots[f"trim-{finding_type}"] = {
                    "schema_version": 4,
                    "pass": False,
                    "checks": [{"id": "trim", "status": "incomplete"}],
                    "errors": [],
                    "warnings": [
                        {
                            "check_id": "trim",
                            "type": finding_type,
                            "severity": "info",
                        }
                    ],
                }
                with patched_corpus(manifest, snapshots, synthetic_overlay({})):
                    with self.assertRaisesRegex(
                        ValueError,
                        "covered check 'trim' has no corpus fixture exercising it",
                    ):
                        self.map()

    def test_informational_findings_with_ok_status_are_not_clean(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots(exclude="trim")
        manifest.append(
            {
                "id": "trim-informational",
                "pdf": "trim-informational.pdf",
                "profile": "profiles/loop-default.json",
                "expect": {"pass": True, "check_ids": ["trim"]},
            }
        )
        snapshots["trim-informational"] = {
            "schema_version": 4,
            "pass": True,
            "checks": [{"id": "trim", "status": "ok"}],
            "errors": [],
            "warnings": [
                {
                    "check_id": "trim",
                    "type": "trim-note",
                    "severity": "info",
                }
            ],
        }
        with patched_corpus(manifest, snapshots, synthetic_overlay({})):
            coverage = self.map()
        self.assertEqual(coverage["rows"]["trim"]["clean_fixture_count"], 0)
        self.assertIn("trim-informational", coverage["rows"]["trim"]["finding_fixtures"])

    def test_uninspected_fixtures_are_recorded_apart_from_findings(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        snapshots["exercises-ink-coverage"]["checks"] = [{"id": "ink-coverage", "status": "incomplete"}]
        snapshots["exercises-ink-coverage"]["errors"] = []
        snapshots["exercises-ink-coverage"]["warnings"] = [
            {"check_id": "ink-coverage", "type": "check-incomplete", "severity": "info"}
        ]
        overlay = synthetic_overlay({"ink-coverage": "inspection did not complete"})
        with patched_corpus(manifest, snapshots, overlay):
            coverage = self.map()
        self.assertEqual(coverage["rows"]["ink-coverage"]["finding_fixtures"], [])
        self.assertEqual(
            coverage["rows"]["ink-coverage"]["uninspected_fixtures"], ["exercises-ink-coverage"]
        )
        self.assertEqual(coverage["corpus_gaps"], ["ink-coverage"])

    def test_overlay_without_a_row_for_a_registered_check_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        overlay = synthetic_overlay({})
        overlay["checks"].pop("trim")
        with patched_corpus(manifest, snapshots, overlay):
            with self.assertRaisesRegex(ValueError, "catalog overlay has no row for: trim"):
                self.map()

    def test_engine_matrix_id_drift_fails(self) -> None:
        manifest, snapshots = synthetic_manifest_and_snapshots()
        overlay = synthetic_overlay({})
        overlay["matrix_id"] = "loop-gwg-pdfx-v9"
        with patched_corpus(manifest, snapshots, overlay):
            with self.assertRaisesRegex(
                ValueError,
                "stamps matrix_id 'loop-gwg-pdfx-v1' but the overlay publishes 'loop-gwg-pdfx-v9'",
            ):
                self.map()


if __name__ == "__main__":
    unittest.main()
