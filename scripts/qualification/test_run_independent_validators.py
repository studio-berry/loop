from __future__ import annotations

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


if __name__ == "__main__":
    unittest.main()
