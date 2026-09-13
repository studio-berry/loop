from __future__ import annotations

import unittest

from scripts.qualification.validate_resource_envelope_evidence import validate_evidence


class ValidateResourceEnvelopeEvidenceTest(unittest.TestCase):
    def test_incomplete_evidence_is_valid(self) -> None:
        evidence = {
            "schema_kind": "loop-resource-envelope-qualification-evidence",
            "schema_version": 1,
            "candidate_sha": "6e65be48e261d14c64653694320c0185b84fc560",
            "disposition": "incomplete",
            "fixture_status": {
                "office-2mb": {"status": "failed", "preflight_high_water_bytes": -1},
                "image-heavy-500mb": {"status": "unavailable"},
                "ten-thousand-page": {"status": "unavailable"},
                "pathological-vector": {"status": "failed", "preflight_high_water_bytes": -1},
                "transparency-spots": {"status": "failed", "preflight_high_water_bytes": -1},
            },
            "matrix_runs": [{"candidate_sha": "6e65be48e261d14c64653694320c0185b84fc560", "summary": {}}],
            "verifiers": [{"command": "test", "result": "pass"}],
        }
        self.assertEqual(validate_evidence(evidence), [])

    def test_passed_disposition_is_rejected(self) -> None:
        evidence = {
            "schema_kind": "loop-resource-envelope-qualification-evidence",
            "schema_version": 1,
            "candidate_sha": "6e65be48e261d14c64653694320c0185b84fc560",
            "disposition": "passed",
            "fixture_status": {
                "office-2mb": {"status": "unavailable"},
                "image-heavy-500mb": {"status": "unavailable"},
                "ten-thousand-page": {"status": "unavailable"},
                "pathological-vector": {"status": "unavailable"},
                "transparency-spots": {"status": "unavailable"},
            },
            "matrix_runs": [{"candidate_sha": "6e65be48e261d14c64653694320c0185b84fc560", "summary": {}}],
            "verifiers": [{"command": "test", "result": "pass"}],
        }
        self.assertIn("must not claim passed", "\n".join(validate_evidence(evidence)))


if __name__ == "__main__":
    unittest.main()
