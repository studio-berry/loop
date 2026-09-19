#!/usr/bin/env python3
"""Unit tests for the governed publication parity checker."""

from __future__ import annotations

import unittest

try:
    from .check_governed_parity import validate_governed
except ImportError:
    from check_governed_parity import validate_governed


DIGEST = "a" * 64


def signed_off_record() -> dict:
    return {
        "approval": {
            "plan_digest": DIGEST,
            "source_sha256": DIGEST,
            "candidate_sha256": DIGEST,
        },
        "revalidation": {
            "artifact_sha256": DIGEST,
            "report_sha256": DIGEST,
            "effective_profile_digest": DIGEST,
            "bytes_verified": True,
            "sign_off_eligible": True,
            "verdict": {"state": "pass"},
        },
        "sign_off": {
            "plan_digest": DIGEST,
            "source_sha256": DIGEST,
            "candidate_sha256": DIGEST,
            "published_sha256": DIGEST,
            "revalidation_report_sha256": DIGEST,
            "effective_profile_digest": DIGEST,
            "approval": {"actorId": "PdfTool", "policyId": "test", "decision": "approve"},
        },
    }


class GovernedParityTest(unittest.TestCase):
    def test_accepts_complete_identity_chain(self) -> None:
        errors, identity = validate_governed(signed_off_record(), "fixture", False)
        self.assertEqual(errors, [])
        self.assertEqual(identity, (DIGEST, DIGEST, DIGEST))

    def test_rejects_published_digest_drift(self) -> None:
        record = signed_off_record()
        record["sign_off"]["published_sha256"] = "b" * 64
        errors, identity = validate_governed(record, "fixture", False)
        self.assertIsNone(identity)
        self.assertTrue(any("published_sha256" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
