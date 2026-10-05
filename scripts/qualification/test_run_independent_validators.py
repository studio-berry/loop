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
            mock.Mock(returncode=0, stdout=b"No syntax or stream encoding errors found\n", stderr=b""),
        ]
        result = validators.run(self.input_path, ["structural"], 1000, "a" * 40)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["candidate_sha"], "a" * 40)
        self.assertEqual(result["input"]["bytes"], len(b"%PDF-1.7\nfixture\n"))
        self.assertEqual(len(result["input"]["sha256"]), 64)
        self.assertEqual(result["validators"][0]["arguments"][-1], str(self.input_path))

    @mock.patch.object(validators, "which", return_value=None)
    def test_missing_validator_is_incomplete(self, _which: mock.Mock) -> None:
        result = validators.run(self.input_path, ["structural"], 1000, "a" * 40)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["validators"][0]["reason_code"], "validator-not-installed")

    @mock.patch.object(validators, "which", return_value="/opt/bin/qpdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_nonzero_validator_is_rejected(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"qpdf version 11.0\n", stderr=b""),
            mock.Mock(returncode=2, stdout=b"", stderr="破損\n".encode()),
        ]
        result = validators.run(self.input_path, ["structural"], 1000, "a" * 40)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["validators"][0]["reason_code"], "validator-rejected")
        self.assertIn("破損", result["validators"][0]["stderr"])

    @mock.patch.object(validators, "which", return_value="/opt/bin/pdfsig")
    @mock.patch.object(validators.subprocess, "run")
    def test_signature_without_signature_is_incomplete(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"pdfsig version 23.1.0\n", stderr=b""),
            mock.Mock(returncode=0, stdout=b"No signatures found\n", stderr=b""),
        ]
        result = validators.run(self.input_path, ["signature"], 1000, "a" * 40)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["validators"][0]["reason_code"], "signature-not-present")

    @mock.patch.object(validators, "which", return_value="/opt/bin/qpdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_timeout_is_incomplete(self, run_mock: mock.Mock, _which: mock.Mock) -> None:
        run_mock.side_effect = [
            mock.Mock(returncode=0, stdout=b"qpdf version 11.0\n", stderr=b""),
            validators.subprocess.TimeoutExpired(["qpdf"], 1, output=b"partial", stderr=b""),
        ]
        result = validators.run(self.input_path, ["structural"], 1, "a" * 40)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["validators"][0]["reason_code"], "validator-timeout")

    def test_json_is_serializable(self) -> None:
        with mock.patch.object(validators, "which", return_value=None):
            evidence = validators.run(self.input_path, ["structural", "standards"], 1000, "a" * 40)
        json.dumps(evidence)

    def test_source_sha_and_nonempty_claims_are_required(self):
        for claims, sha in [([], "a" * 40), (["standards"], "abc123"), (["unknown"], "a" * 40)]:
            with self.assertRaises(ValueError):
                validators.run(self.input_path, claims, 1000, sha)

    def test_standard_report_normalizes_native_separators(self):
        name = str(self.input_path).replace("\\", "/")
        self.assertEqual(validators.parse_standard_report(self.standard_report(name=name), self.input_path)[0], "passed")
        self.assertEqual(validators.parse_standard_report(self.standard_report(name="candidate.pdf"), self.input_path)[0], "incomplete")
        windows = Path("C:/qualification/candidate.pdf")
        self.assertEqual(validators.parse_standard_report(self.standard_report(name=r"C:\qualification\candidate.pdf"), windows)[0], "passed")

    @mock.patch.object(validators.subprocess, "run")
    def test_qpdf_windows_executable_name_is_recognized(self, run_mock):
        run_mock.return_value = mock.Mock(returncode=0, stdout=b"qpdf.EXE version 12.4.2\r\n", stderr=b"")
        self.assertEqual(validators._version("qpdf.EXE", "structural"), "qpdf.EXE version 12.4.2")
    def standard_report(self, compliant="true", profile="PDF/A-2B validation profile", name=None):
        root = validators.ET.Element("report")
        job = validators.ET.SubElement(validators.ET.SubElement(root, "jobs"), "job")
        validators.ET.SubElement(validators.ET.SubElement(job, "item"), "name").text = str(name or self.input_path)
        report = validators.ET.SubElement(job, "validationReport", profileName=profile, isCompliant=compliant, jobEndStatus="normal")
        validators.ET.SubElement(report, "details", passedRules="102", failedRules="0", passedChecks="504", failedChecks="0")
        summary = validators.ET.SubElement(root, "batchSummary", totalJobs="1", failedToParse="0", encrypted="0", outOfMemory="0", veraExceptions="0")
        validators.ET.SubElement(summary, "validationReports", compliant="1", nonCompliant="0", failedJobs="0").text = "1"
        return validators.ET.tostring(root)

    @mock.patch.object(validators, "which", return_value="/opt/bin/verapdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_standard_verdict_comes_from_report(self, run_mock, _which):
        for compliant, expected in [("true", "passed"), ("false", "rejected"), ("unknown", "incomplete")]:
            run_mock.side_effect = [mock.Mock(returncode=0, stdout=b"veraPDF 1.28.2", stderr=b""),
                                    mock.Mock(returncode=0, stdout=self.standard_report(compliant), stderr=b"")]
            result = validators.run(self.input_path, ["standards"], 1000, "a" * 40)
            self.assertEqual(result["status"], expected)
            self.assertEqual(result["validators"][0]["report_sha256"], hashlib.sha256(self.standard_report(compliant)).hexdigest())

    def test_wrong_profile_input_and_malformed_reports_cannot_pass(self):
        for raw in [self.standard_report(profile="PDF/A-1B validation profile"),
                    self.standard_report(name=Path("other.pdf")), b"", b"<report/>",
                    self.standard_report().replace(b'normal', b'failed'),
                    self.standard_report().replace(b'failedChecks="0"', b'failedChecks="1"'),
                    self.standard_report().replace(b'totalJobs="1"', b'totalJobs="2"'),
                    self.standard_report().replace(b'passedRules="102"', b'passedRules="0"')]:
            status, _ = validators.parse_standard_report(raw, self.input_path)
            self.assertEqual(status, "incomplete")

    def test_mixed_signature_results_and_unknown_trust_are_separate(self):
        valid = b"Signature #1:\n - Signed Ranges: [0 - 123], [456 - 789]\n - Not total document signed\n - Signature Validation: Signature is Valid.\n - Certificate Validation: Certificate issuer is unknown.\n"
        status, _, records = validators.parse_signature_report(valid)
        self.assertEqual(status, "passed")
        self.assertEqual(records[0]["coverage"], "Not total document signed")
        self.assertIn("unknown", records[0]["certificate_trust"])
        invalid = valid.replace(b"#1", b"#2").replace(b"Signature is Valid.", b"Digest Mismatch.")
        self.assertEqual(validators.parse_signature_report(valid + invalid)[0], "rejected")
        self.assertEqual(validators.parse_signature_report(b"Signature #1:\n")[0], "incomplete")

    def test_signature_revision_ranges_must_be_complete_and_consistent(self):
        report = b"Signature #1:\n - Signed Ranges: [0 - 123], [456 - 789]\n - Not total document signed\n - Signature Validation: Signature is Valid.\n"
        self.assertEqual(validators.parse_signature_report(report, 900)[0], "passed")
        self.assertEqual(validators.parse_signature_report(report, 789)[0], "incomplete")
        self.assertEqual(validators.parse_signature_report(report.replace(b"[0 - 123]", b"[100 - 123]"), 900)[0], "incomplete")
        self.assertEqual(validators.parse_signature_report(report.replace(b"#1", b"#2"), 900)[0], "incomplete")
        self.assertEqual(validators.parse_signature_report(report.replace(b"[456 - 789]", b"[100 - 789]"), 900)[0], "incomplete")

    @mock.patch.object(validators, "which", return_value="/opt/bin/verapdf")
    @mock.patch.object(validators.subprocess, "run", side_effect=KeyboardInterrupt)
    def test_cancelled_version_probe_is_incomplete(self, _run, _which):
        evidence = validators.run(self.input_path, ["standards"], 1000, "a" * 40)
        self.assertEqual(evidence["status"], "incomplete")
        self.assertEqual(evidence["validators"][0]["reason_code"], "validator-cancelled")

    @mock.patch.object(validators, "which", return_value="/opt/bin/qpdf")
    @mock.patch.object(validators.subprocess, "run")
    def test_unknown_version_and_mutation_are_not_passes(self, run_mock, _which):
        run_mock.return_value = mock.Mock(returncode=0, stdout=b"unknown version", stderr=b"")
        self.assertEqual(validators.run(self.input_path, ["structural"], 1000, "a" * 40)["status"], "incomplete")
        def change_input(*args, **kwargs):
            self.input_path.write_bytes(b"changed")
            return mock.Mock(returncode=0, stdout=b"No syntax or stream encoding errors found", stderr=b"")
        calls = 0
        def version_then_change(*args, **kwargs):
            nonlocal calls
            calls += 1
            return mock.Mock(returncode=0, stdout=b"qpdf version 11.0", stderr=b"") if calls == 1 else change_input()
        run_mock.side_effect = version_then_change
        self.assertEqual(validators.run(self.input_path, ["structural"], 1000, "a" * 40)["status"], "rejected")

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
        evidence = validators.run(self.input_path, ["structural"], 1000, "a" * 40, bundle)
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
        evidence = validators.run(self.input_path, ["structural"], 1000, "a" * 40, bundle)
        self.assertEqual(evidence["bundle"]["status"], "rejected")
        self.assertEqual(evidence["bundle"]["reason_code"], "member-digest-mismatch")
        self.assertEqual(evidence["status"], "rejected")

    @mock.patch.object(validators, "which", return_value=None)
    def test_bundle_content_outside_the_declared_set_is_rejected(self, _which: mock.Mock) -> None:
        _size, digest = validators._input_identity(self.input_path)
        bundle = self._write_bundle(digest)
        (bundle / "notes.json").write_text("{}", encoding="utf-8")
        evidence = validators.run(self.input_path, ["structural"], 1000, "a" * 40, bundle)
        self.assertEqual(evidence["bundle"]["status"], "rejected")
        self.assertEqual(evidence["bundle"]["reason_code"], "member-undeclared")
        self.assertEqual(evidence["bundle"]["undeclared_members"], ["notes.json"])

    @mock.patch.object(validators, "which", return_value=None)
    def test_bundle_for_a_different_revision_is_rejected(self, _which: mock.Mock) -> None:
        bundle = self._write_bundle("f" * 64)
        evidence = validators.run(self.input_path, ["structural"], 1000, "a" * 40, bundle)
        self.assertEqual(evidence["bundle"]["status"], "rejected")
        self.assertEqual(evidence["bundle"]["reason_code"], "input-does-not-match-manifest")
        self.assertFalse(evidence["bundle"]["input_matches_manifest"])

    @mock.patch.object(validators, "which", return_value=None)
    def test_missing_bundle_is_incomplete_not_a_pass(self, _which: mock.Mock) -> None:
        evidence = validators.run(self.input_path, ["structural"], 1000, "a" * 40,
                                  Path(self.tempdir.name) / "absent")
        self.assertEqual(evidence["bundle"]["status"], "incomplete")
        self.assertEqual(evidence["bundle"]["reason_code"], "bundle-not-found")
        self.assertEqual(evidence["status"], "incomplete")

    @mock.patch.object(validators, "which", return_value=None)
    def test_evidence_without_a_bundle_carries_no_bundle_block(self, _which: mock.Mock) -> None:
        evidence = validators.run(self.input_path, ["structural"], 1000, "a" * 40)
        self.assertNotIn("bundle", evidence)


if __name__ == "__main__":
    unittest.main()
