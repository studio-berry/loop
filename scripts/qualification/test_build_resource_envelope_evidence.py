from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from scripts.qualification.build_resource_envelope_evidence import build_evidence
from scripts.qualification.validate_resource_envelope_evidence import validate_evidence
from scripts.resource_envelope.run_matrix import FIXTURE_SPECS

CANDIDATE = "a" * 40


def _matrix(failed_fixture: str | None = None) -> dict:
    fixtures = []
    for fixture_id, spec in FIXTURE_SPECS.items():
        if not spec["required"]:
            fixtures.append({"fixture_id": fixture_id, "status": "unavailable", "required": False})
            continue
        status = "failed" if fixture_id == failed_fixture else "measured"
        fixtures.append({
            "fixture_id": fixture_id,
            "status": status,
            "required": True,
            "fixture_sha256": "b" * 64,
            "identity": {"os": "test-os", "qt": "6.11.1"},
            "result": {"status": "complete", "rss_high_water_bytes": 100, "preflight_high_water_bytes": 90, "elapsed_ms": 10, "pages_materialized": 1, "page_count": 1},
        })
    failed = int(failed_fixture is not None)
    return {
        "candidate_sha": CANDIDATE,
        "fixtures": fixtures,
        "cancellation_recovery_probe": {"status": "measured", "fixture_id": "ten-thousand-page", "cancellation": {"cancellation_latency_ms": 40}, "recovery": {"recovery_ms": 900}},
        "hostile": {"summary": {"total": 7, "contained": 7}, "cases": []},
        "summary": {"total": len(fixtures), "measured": len(fixtures) - 1 - failed, "flagged": 0, "skipped": 1, "failed": failed, "candidate_sha_verified": True},
    }


class BuildResourceEnvelopeEvidenceTest(unittest.TestCase):
    def _build(self, matrices: dict[str, dict]) -> dict:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {}
            for platform, matrix in matrices.items():
                paths[platform] = root / f"{platform}.json"
                paths[platform].write_text(json.dumps(matrix), encoding="utf-8")
            manifest = root / "fixtures.json"
            manifest.write_text(json.dumps({"generator": {"version": 1}}), encoding="utf-8")
            return build_evidence(paths, manifest, "123456", "https://github.com/studio-berry/loop/actions/runs/123456")

    def test_both_platforms_passing_is_valid_passed_evidence(self) -> None:
        evidence = self._build({"linux": _matrix(), "windows": _matrix()})
        self.assertEqual(evidence["disposition"], "passed", evidence["disposition_reasons"])
        self.assertEqual(validate_evidence(evidence), [])

    def test_missing_platform_is_incomplete(self) -> None:
        evidence = self._build({"linux": _matrix()})
        self.assertEqual(evidence["disposition"], "incomplete")
        self.assertIn("platform windows produced no matrix", evidence["disposition_reasons"])
        self.assertEqual(validate_evidence(evidence), [])

    def test_failed_fixture_is_rejected(self) -> None:
        evidence = self._build({"linux": _matrix(), "windows": _matrix(failed_fixture="image-heavy-500mb")})
        self.assertEqual(evidence["disposition"], "rejected")
        self.assertEqual(validate_evidence(evidence), [])

    def test_passed_claim_with_unavailable_measurement_is_invalid(self) -> None:
        evidence = self._build({"linux": _matrix(), "windows": _matrix()})
        tampered = copy.deepcopy(evidence)
        tampered["platforms"]["linux"]["fixtures"]["office-2mb"]["envelope"]["preflight_high_water_bytes"] = -1
        tampered["platforms"]["windows"]["cancellation_recovery_probe"]["recovery_ms"] = -1
        errors = "\n".join(validate_evidence(tampered))
        self.assertIn("platforms.linux.fixtures.office-2mb.preflight_high_water_bytes is unavailable", errors)
        self.assertIn("platforms.windows.cancellation_recovery_probe.recovery_ms is unavailable", errors)


if __name__ == "__main__":
    unittest.main()
