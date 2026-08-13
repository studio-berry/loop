#!/usr/bin/env python3
import copy
import json
import unittest
from pathlib import Path

from scripts.ci.check_supply_chain_pins import (
    FULL_SHA,
    HEX_DIGEST,
    validate_packaging_pins_data,
    validate_source_text,
    validate_vcpkg_data,
    validate_workflow_text,
)


ROOT = Path(__file__).resolve().parents[2]


class SupplyChainPolicyTests(unittest.TestCase):
    def test_rejects_mutable_action_tag(self):
        violations = validate_workflow_text(Path("ci.yml"), "uses: actions/checkout@v4\n")
        self.assertTrue(any("full commit SHA" in violation.message for violation in violations))

    def test_accepts_full_action_sha(self):
        workflow = "uses: actions/checkout@34e114876b0b11c390a56381ad16ebd13914f8d5\n"
        self.assertEqual(validate_workflow_text(Path("ci.yml"), workflow), [])

    def test_rejects_mutable_release_download_url(self):
        text = "run: curl -LO https://example.invalid/releases/latest/download/tool\n"
        violations = validate_source_text(Path("ci.yml"), text)
        self.assertTrue(any("mutable release download URL" in violation.message for violation in violations))

    def test_rejects_mutable_continuous_download_in_script(self):
        text = "curl https://example.invalid/releases/download/continuous/tool\n"
        self.assertTrue(validate_source_text(Path("download.sh"), text))

    def test_accepts_current_vcpkg_configuration(self):
        config = json.loads((ROOT / "vcpkg-configuration.json").read_text(encoding="utf-8"))
        self.assertEqual(validate_vcpkg_data(Path("vcpkg-configuration.json"), config), [])

    def test_rejects_non_sha_vcpkg_baseline(self):
        violations = validate_vcpkg_data(Path("vcpkg-configuration.json"), {"default-registry": {"baseline": "main"}})
        self.assertTrue(any("full commit SHA" in violation.message for violation in violations))

    def test_rejects_bad_packaging_digest(self):
        pins = json.loads((ROOT / ".github/pins/packaging-tools.json").read_text(encoding="utf-8"))
        pins = copy.deepcopy(pins)
        pins["wix"]["sha256"] = "not-a-digest"
        violations = validate_packaging_pins_data(Path("packaging-tools.json"), pins)
        self.assertTrue(any("wix.sha256" in violation.message for violation in violations))

    def test_rejects_partial_keylocker_pin(self):
        pins = json.loads((ROOT / ".github/pins/packaging-tools.json").read_text(encoding="utf-8"))
        pins = copy.deepcopy(pins)
        pins["digicertKeylocker"]["version"] = "1.0.0"
        violations = validate_packaging_pins_data(Path("packaging-tools.json"), pins)
        self.assertTrue(any("must be set together" in violation.message for violation in violations))

    def test_rejects_missing_sentry_cli_pin(self):
        pins = json.loads((ROOT / ".github/pins/packaging-tools.json").read_text(encoding="utf-8"))
        pins = copy.deepcopy(pins)
        del pins["sentryCli"]
        violations = validate_packaging_pins_data(Path("packaging-tools.json"), pins)
        self.assertTrue(any("sentryCli" in violation.message for violation in violations))

    def test_policy_constants_are_strict(self):
        self.assertTrue(FULL_SHA.fullmatch("a" * 40))
        self.assertIsNone(FULL_SHA.fullmatch("a" * 39))
        self.assertTrue(HEX_DIGEST.fullmatch("a" * 64))
        self.assertIsNone(HEX_DIGEST.fullmatch("a" * 63))


if __name__ == "__main__":
    unittest.main()
