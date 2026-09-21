from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.qualification import run_independent_validators as validators


class IndependentValidatorTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.input_path = Path(self.tempdir.name) / "candidate.pdf"
        self.input_path.write_bytes(b"%PDF-1.7\nfixture\n")

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    @mock.patch.object(validators, "which", return_value="/opt/bin/qpdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_pass_records_identity_and_invocation(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"qpdf version 11.0\n", stderr=b""),
            mock.Mock(returncode=0, stdout=b"checking\n", stderr=b""),
        ]
        result = validators.run(self.input_path, ["structural"], 1000, "abc123")
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["candidate_sha"], "abc123")
        self.assertEqual(result["input"]["bytes"], len(b"%PDF-1.7\nfixture\n"))
        self.assertEqual(len(result["input"]["sha256"]), 64)
        self.assertEqual(result["validators"][0]["arguments"][-1], str(self.input_path))

    @mock.patch.object(validators, "which", return_value=None)
    def test_missing_validator_is_incomplete(self, _which: mock.Mock) -> None:
        result = validators.run(self.input_path, ["structural"], 1000)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["validators"][0]["reason_code"], "validator-not-installed")

    @mock.patch.object(validators, "which", return_value="/opt/bin/qpdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_nonzero_validator_is_rejected(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"qpdf version 11.0\n", stderr=b""),
            mock.Mock(returncode=2, stdout=b"", stderr="破損\n".encode()),
        ]
        result = validators.run(self.input_path, ["structural"], 1000)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["validators"][0]["reason_code"], "validator-rejected")
        self.assertIn("破損", result["validators"][0]["stderr"])

    @mock.patch.object(validators, "which", return_value="/opt/bin/pdfsig")
    @mock.patch.object(validators.subprocess, "run")
    def test_signature_without_signature_is_incomplete(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"pdfsig version 23\n", stderr=b""),
            mock.Mock(returncode=0, stdout=b"No signatures found\n", stderr=b""),
        ]
        result = validators.run(self.input_path, ["signature"], 1000)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["validators"][0]["reason_code"], "signature-not-present")

    @mock.patch.object(validators, "which", return_value="/opt/bin/qpdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_timeout_is_incomplete(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"qpdf version 11.0\n", stderr=b""),
            validators.subprocess.TimeoutExpired(["qpdf"], 1, output=b"partial", stderr=b""),
        ]
        result = validators.run(self.input_path, ["structural"], 1)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["validators"][0]["reason_code"], "validator-timeout")

    def test_json_is_serializable(self) -> None:
        with mock.patch.object(validators, "which", return_value=None):
            evidence = validators.run(self.input_path, ["structural", "standards"], 1000)
        json.dumps(evidence)

    def _write_bundle(self, digest: str, member_bytes: bytes = b'{"report": "fixture"}') -> Path:
        bundle = Path(self.tempdir.name) / "bundle"
        bundle.mkdir()
        (bundle / "report.json").write_bytes(member_bytes)
        manifest = {
            "schema": validators.BUNDLE_SCHEMA,
            "schema_version": validators.BUNDLE_SCHEMA_VERSION,
            "document": {"revision_digest": digest, "byte_count": 18, "source_path_included": False},
            "members": [
                {
                    "name": "report.json",
                    "media_type": "application/json",
                    "sha256": hashlib.sha256(member_bytes).hexdigest(),
                    "byte_count": len(member_bytes),
                }
            ],
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return bundle

    @mock.patch.object(validators, "which", return_value=None)
    def test_bundle_must_verify_and_match_the_candidate(self, _which: mock.Mock) -> None:
        _size, digest = validators._input_identity(self.input_path)
        bundle = self._write_bundle(digest)
        evidence = validators.run(self.input_path, ["structural"], 1000, None, bundle)
        self.assertEqual(evidence["bundle"]["status"], "passed")
        self.assertEqual(evidence["bundle"]["members_verified"], 1)
        self.assertTrue(evidence["bundle"]["input_matches_manifest"])
        # The validators themselves are still missing on this host, so the lane
        # stays incomplete rather than reporting a pass.
        self.assertEqual(evidence["status"], "incomplete")

    @mock.patch.object(validators, "which", return_value=None)
    def test_bundle_member_tampering_is_rejected(self, _which: mock.Mock) -> None:
        _size, digest = validators._input_identity(self.input_path)
        bundle = self._write_bundle(digest)
        (bundle / "report.json").write_bytes(b'{"report": "tampered"}')
        evidence = validators.run(self.input_path, ["structural"], 1000, None, bundle)
        self.assertEqual(evidence["bundle"]["status"], "rejected")
        self.assertEqual(evidence["bundle"]["reason_code"], "member-digest-mismatch")
        self.assertEqual(evidence["status"], "rejected")

    @mock.patch.object(validators, "which", return_value=None)
    def test_bundle_content_outside_the_declared_set_is_rejected(self, _which: mock.Mock) -> None:
        _size, digest = validators._input_identity(self.input_path)
        bundle = self._write_bundle(digest)
        (bundle / "notes.json").write_text("{}", encoding="utf-8")
        evidence = validators.run(self.input_path, ["structural"], 1000, None, bundle)
        self.assertEqual(evidence["bundle"]["status"], "rejected")
        self.assertEqual(evidence["bundle"]["reason_code"], "member-undeclared")
        self.assertEqual(evidence["bundle"]["undeclared_members"], ["notes.json"])

    @mock.patch.object(validators, "which", return_value=None)
    def test_bundle_for_a_different_revision_is_rejected(self, _which: mock.Mock) -> None:
        bundle = self._write_bundle("f" * 64)
        evidence = validators.run(self.input_path, ["structural"], 1000, None, bundle)
        self.assertEqual(evidence["bundle"]["status"], "rejected")
        self.assertEqual(evidence["bundle"]["reason_code"], "input-does-not-match-manifest")
        self.assertFalse(evidence["bundle"]["input_matches_manifest"])

    @mock.patch.object(validators, "which", return_value=None)
    def test_missing_bundle_is_incomplete_not_a_pass(self, _which: mock.Mock) -> None:
        evidence = validators.run(self.input_path, ["structural"], 1000, None,
                                  Path(self.tempdir.name) / "absent")
        self.assertEqual(evidence["bundle"]["status"], "incomplete")
        self.assertEqual(evidence["bundle"]["reason_code"], "bundle-not-found")
        self.assertEqual(evidence["status"], "incomplete")

    @mock.patch.object(validators, "which", return_value=None)
    def test_evidence_without_a_bundle_carries_no_bundle_block(self, _which: mock.Mock) -> None:
        evidence = validators.run(self.input_path, ["structural"], 1000)
        self.assertNotIn("bundle", evidence)


if __name__ == "__main__":
    unittest.main()
