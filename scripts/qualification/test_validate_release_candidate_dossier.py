from __future__ import annotations

import unittest

from scripts.qualification import validate_release_candidate_dossier as validator


class ReleaseCandidateDossierTest(unittest.TestCase):
    candidate_sha = "a" * 40

    def dossier(self) -> dict:
        return {
            "candidate_sha": self.candidate_sha,
            "artifacts": [{"name": "Loop.AppImage", "sha256": "b" * 64, "built_from_sha": self.candidate_sha}],
        }

    def test_matching_artifact_provenance_passes(self) -> None:
        self.assertEqual(validator.validate_dossier(self.dossier()), [])

    def test_missing_artifact_provenance_is_rejected(self) -> None:
        dossier = self.dossier()
        del dossier["artifacts"][0]["built_from_sha"]
        errors = validator.validate_dossier(dossier)
        self.assertTrue(any("built_from_sha" in error for error in errors))

    def test_mismatched_artifact_provenance_is_rejected(self) -> None:
        dossier = self.dossier()
        dossier["artifacts"][0]["built_from_sha"] = "c" * 40
        self.assertIn("artifacts[0].built_from_sha must equal candidate_sha", validator.validate_dossier(dossier))


if __name__ == "__main__":
    unittest.main()
