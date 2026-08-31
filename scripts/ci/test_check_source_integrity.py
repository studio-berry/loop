#!/usr/bin/env python3
"""Tests for the tracked-source integrity check.

Conflict markers are assembled from fragments on purpose: a literal marker in
this file would make check_source_integrity.py flag the test itself.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from scripts.ci.check_source_integrity import (  # noqa: E402
    MAX_TRACKED_BYTES,
    forbidden_path_reason,
    fuzz_corpus_violations,
    has_conflict_markers,
    oversized_reason,
    preflight_pdf_violations,
    validate_repository,
    whitespace_violations,
)

OURS = "<" * 7
THEIRS = ">" * 7
DIVIDER = "=" * 7


class ForbiddenPathTests(unittest.TestCase):
    def test_rejects_build_trees(self):
        for path in (
            "build/CMakeFiles/rules.ninja",
            "build-fuzz-docker/usr/lib/libLoopLibCore.so.1.6.0.0",
            "build-fuzz-docker/.ninja_log",
            ".docker-vcpkg",
            ".docker-vcpkg/installed/x64-linux/include/zlib.h",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(forbidden_path_reason(path))

    def test_rejects_build_caches_anywhere(self):
        for path in ("CMakeCache.txt", "some/nested/dir/CMakeCache.txt"):
            with self.subTest(path=path):
                self.assertIsNotNone(forbidden_path_reason(path))

    def test_rejects_one_off_debug_artifacts(self):
        for path in (
            "debug-b0e75b.log",
            "debug-pr188.log",
            "scripts/debug-pr188.sh",
            "scripts/debug-pr188.ps1",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(forbidden_path_reason(path))

    def test_keeps_legitimate_sources(self):
        for path in (
            "Fuzz/fuzz_pdf_parser.cpp",
            "Fuzz/corpus/fuzz_images/jbig2-composition-timeout.bin",
            "scripts/ci/check_source_integrity.py",
            "scripts/hooks/cc-guard-bash.sh",
            "build-notes.md",
            "buildsystem/toolchain.cmake",
            "LoopLibCore/sources/pdfdocument.cpp",
            "docs/adr/adr-003-pagemaster-export-orchestrator.md",
            "loop-preflight/testdata/fixtures/image-dpi-excessive.pdf",
        ):
            with self.subTest(path=path):
                self.assertIsNone(forbidden_path_reason(path))

    def test_normalizes_windows_separators(self):
        self.assertIsNotNone(forbidden_path_reason("build-fuzz-docker\\config.h"))


class ConflictMarkerTests(unittest.TestCase):
    def test_detects_markers(self):
        text = "\n".join(
            [
                "/// Headless PageMaster export orchestrator (ADR-003).",
                f"{OURS} HEAD",
                "/// Locked stage order: assemble.",
                DIVIDER,
                "/// Locked stage order: assemble, flatten.",
                f"{THEIRS} origin/dev",
            ]
        )
        self.assertTrue(has_conflict_markers(text))

    def test_detects_bare_marker_line(self):
        self.assertTrue(has_conflict_markers(f"line\n{OURS}\nline"))

    def test_ignores_markdown_and_prose(self):
        for text in (
            f"Heading\n{DIVIDER}\n\nbody text",
            "shift left with a << b and right with c >> d",
            f"{'<' * 8} eight is not a marker",
            "a line that merely mentions a conflict marker",
        ):
            with self.subTest(text=text[:32]):
                self.assertFalse(has_conflict_markers(text))


class OversizedFileTests(unittest.TestCase):
    def test_accepts_files_at_or_under_the_cap(self):
        self.assertIsNone(oversized_reason("fixture.pdf", MAX_TRACKED_BYTES))
        self.assertIsNone(oversized_reason("fixture.pdf", 1_440_774))

    def test_rejects_files_over_the_cap(self):
        reason = oversized_reason("build-fuzz-docker/lib.so", MAX_TRACKED_BYTES + 1)
        self.assertIsNotNone(reason)
        self.assertIn("cap", reason)

    def test_allowlist_exempts_a_named_file(self):
        from scripts.ci import check_source_integrity

        original = dict(check_source_integrity.LARGE_FILE_ALLOWLIST)
        check_source_integrity.LARGE_FILE_ALLOWLIST["huge/fixture.pdf"] = "why"
        try:
            self.assertIsNone(
                oversized_reason("huge/fixture.pdf", MAX_TRACKED_BYTES + 1)
            )
        finally:
            check_source_integrity.LARGE_FILE_ALLOWLIST.clear()
            check_source_integrity.LARGE_FILE_ALLOWLIST.update(original)


class RepositoryScanTests(unittest.TestCase):
    """End-to-end scan over a throwaway repository containing each violation."""

    def _git(self, root, *args):
        command = ["git", "-C", str(root)]
        if args and args[0] == "commit":
            # Throwaway repos have no identity, and some runners enable gpgsign.
            command.extend(
                [
                    "-c",
                    "user.email=ci@example.invalid",
                    "-c",
                    "user.name=ci",
                    "-c",
                    "commit.gpgsign=false",
                ]
            )
        command.extend(args)
        completed = subprocess.run(command, capture_output=True)
        if completed.returncode != 0:
            stderr = completed.stderr.decode(errors="replace").strip()
            raise AssertionError(f"git {' '.join(args)} failed: {stderr}")

    def test_reports_each_violation_class(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self._git(root, "init", "-q")

            (root / "clean.txt").write_text("ordinary tracked content\n")

            (root / "build-fuzz-docker").mkdir()
            (root / "build-fuzz-docker" / "config.h").write_text("generated\n")

            (root / "debug-abc123.log").write_text("scratch\n")

            (root / "conflicted.h").write_text(
                f"before\n{OURS} HEAD\nmine\n{DIVIDER}\ntheirs\n{THEIRS} origin/dev\nafter\n"
            )

            (root / "huge.bin").write_bytes(b"\0" * (MAX_TRACKED_BYTES + 1))

            self._git(root, "add", "-A")

            violations = dict(validate_repository(root))

            self.assertIn("build-fuzz-docker/config.h", violations)
            self.assertIn("debug-abc123.log", violations)
            self.assertIn("huge.bin", violations)
            self.assertEqual(
                violations.get("conflicted.h"), "unresolved merge-conflict marker"
            )
            self.assertNotIn("clean.txt", violations)

    def test_clean_repository_reports_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self._git(root, "init", "-q")
            (root / "src").mkdir()
            (root / "src" / "main.cpp").write_text("int main() { return 0; }\n")
            (root / "logo.png").write_bytes(b"\x89PNG\r\n\x1a\n\xff\xfe binary")
            self._git(root, "add", "-A")

            self.assertEqual(validate_repository(root), [])

    def test_reports_trailing_whitespace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self._git(root, "init", "-q")
            (root / "trailing.txt").write_text("line with spaces   \n")
            self._git(root, "add", "-A")
            self._git(root, "commit", "-qm", "add trailing whitespace")

            violations = dict(whitespace_violations(root))
            self.assertIn("trailing.txt", violations)
            self.assertIn("whitespace:", violations["trailing.txt"])

    def test_reports_unmanifested_fuzz_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            corpus = root / "Fuzz" / "corpus" / "regression"
            corpus.mkdir(parents=True)
            (corpus / "manifest.json").write_text(
                json.dumps(
                    {
                        "entries": [
                            {
                                "file": "listed.bin",
                                "rationale": "listed regression seed",
                            }
                        ]
                    }
                )
                + "\n"
            )
            (corpus / "listed.bin").write_bytes(b"ok")
            (corpus / "orphan.bin").write_bytes(b"bad")

            self._git(root, "init", "-q")
            self._git(root, "add", "-A")

            violations = dict(fuzz_corpus_violations(root))
            self.assertIn("Fuzz/corpus/regression/orphan.bin", violations)
            self.assertNotIn("Fuzz/corpus/regression/listed.bin", violations)

    def test_accepts_manifested_fuzz_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            corpus = root / "Fuzz" / "corpus" / "regression"
            corpus.mkdir(parents=True)
            (corpus / "manifest.json").write_text(
                json.dumps(
                    {
                        "entries": [
                            {
                                "file": "listed.bin",
                                "rationale": "listed regression seed",
                            }
                        ]
                    }
                )
                + "\n"
            )
            (corpus / "listed.bin").write_bytes(b"ok")

            self._git(root, "init", "-q")
            self._git(root, "add", "-A")

            self.assertEqual(fuzz_corpus_violations(root), [])

    def test_ignores_harness_corpus_outside_regression(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            images = root / "Fuzz" / "corpus" / "fuzz_images"
            images.mkdir(parents=True)
            (images / "jbig2-timeout.bin").write_bytes(b"seed")
            (root / "Fuzz" / "corpus" / "LICENSE").write_text("license\n")
            regression = root / "Fuzz" / "corpus" / "regression"
            regression.mkdir(parents=True)
            (regression / "manifest.json").write_text(
                json.dumps({"entries": []}) + "\n"
            )

            self._git(root, "init", "-q")
            self._git(root, "add", "-A")

            self.assertEqual(fuzz_corpus_violations(root), [])

    def test_ignores_harness_corpus_outside_regression(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            images = root / "Fuzz" / "corpus" / "fuzz_images"
            images.mkdir(parents=True)
            (images / "jbig2-timeout.bin").write_bytes(b"seed")
            (root / "Fuzz" / "corpus" / "LICENSE").write_text("license\n")
            regression = root / "Fuzz" / "corpus" / "regression"
            regression.mkdir(parents=True)
            (regression / "manifest.json").write_text(
                json.dumps({"entries": []}) + "\n"
            )

            self._git(root, "init", "-q")
            self._git(root, "add", "-A")

            self.assertEqual(fuzz_corpus_violations(root), [])

    def test_reports_unmanifested_preflight_pdf(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixtures = root / "loop-preflight" / "testdata" / "fixtures"
            fixtures.mkdir(parents=True)
            (fixtures / "manifest.json").write_text(
                json.dumps(
                    [
                        {
                            "id": "listed",
                            "pdf": "listed.pdf",
                            "profile": "profiles/loop-default.json",
                            "expect": {"pass": True, "check_ids": []},
                        }
                    ]
                )
                + "\n"
            )
            (fixtures / "listed.pdf").write_bytes(b"%PDF-1.4")
            (fixtures / "orphan.pdf").write_bytes(b"%PDF-1.4")

            self._git(root, "init", "-q")
            self._git(root, "add", "-A")

            violations = dict(preflight_pdf_violations(root))
            self.assertIn(
                "loop-preflight/testdata/fixtures/orphan.pdf", violations
            )
            self.assertNotIn(
                "loop-preflight/testdata/fixtures/listed.pdf", violations
            )


if __name__ == "__main__":
    unittest.main()
