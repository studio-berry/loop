#!/usr/bin/env python3
import unittest
from pathlib import Path

from scripts.ci.check_version_policy import (
    parse_cmake_version,
    parse_policy,
    validate_agents_version,
    validate_appx_manifest,
    validate_release_workflow,
    validate_repository,
    validate_versioning_doc,
)


ROOT = Path(__file__).resolve().parents[2]


class VersionPolicyTests(unittest.TestCase):
    def test_current_repository_matches_policy(self):
        self.assertEqual(validate_repository(ROOT), [])

    def test_policy_requires_semver(self):
        policy = parse_policy((ROOT / "docs" / "version-policy.json").read_text(encoding="utf-8"))
        self.assertEqual(policy["scheme"], "semver")
        self.assertEqual(policy["cmake_format"], "MAJOR.MINOR.PATCH")
        self.assertEqual(policy["tag_prefix"], "v")

    def test_rejects_four_part_cmake_version(self):
        with self.assertRaisesRegex(ValueError, "MAJOR.MINOR.PATCH"):
            parse_cmake_version("set(LOUPE_VERSION 1.6.0.0)\n")

    def test_accepts_three_part_cmake_version(self):
        self.assertEqual(parse_cmake_version("set(LOUPE_VERSION 0.2.0)\n"), "0.2.0")

    def test_rejects_four_part_release_grep(self):
        workflow = (
            'version=$(grep -oP \'set\\(LOUPE_VERSION \\K[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+\' '
            '"CMakeLists.txt")\n'
        )
        errors = validate_release_workflow(workflow)
        self.assertTrue(any("four-part" in error for error in errors))

    def test_accepts_three_part_release_grep(self):
        workflow = (
            'version=$(grep -oP \'set\\(LOUPE_VERSION \\K[0-9]+\\.[0-9]+\\.[0-9]+\' '
            '"CMakeLists.txt")\n'
        )
        self.assertEqual(validate_release_workflow(workflow), [])

    def test_rejects_appx_on_product_version(self):
        errors = validate_appx_manifest(
            'Version="${LOUPE_VERSION}"',
            {"windows_variable": "LOUPE_WINDOWS_VERSION"},
        )
        self.assertTrue(errors)

    def test_accepts_appx_windows_version(self):
        self.assertEqual(
            validate_appx_manifest(
                'Version="${LOUPE_WINDOWS_VERSION}"',
                {"windows_variable": "LOUPE_WINDOWS_VERSION"},
            ),
            [],
        )

    def test_rejects_four_part_agents_version(self):
        errors = validate_agents_version("| **Version** | `1.6.0.0` |", "0.2.0", "alpha")
        self.assertTrue(errors)

    def test_versioning_doc_requires_semver_scheme(self):
        policy = parse_policy((ROOT / "docs" / "version-policy.json").read_text(encoding="utf-8"))
        stale = (ROOT / "docs" / "VERSIONING.md").read_text(encoding="utf-8").replace(
            "Scheme: SemVer 2.0", "Scheme: CalVer"
        )
        errors = validate_versioning_doc(stale, policy)
        self.assertTrue(any("SemVer" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
