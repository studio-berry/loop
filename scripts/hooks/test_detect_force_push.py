#!/usr/bin/env python3
"""Regression tests for the BSP-002 §3.3 force-push detector."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from scripts.hooks.detect_force_push import is_force_push  # noqa: E402


class ForcePushDetectionTests(unittest.TestCase):
    def test_detects_real_force_pushes(self):
        for command in (
            "git push --force origin dev",
            "git push -f origin dev",
            "git push origin dev --force",
            "git push --force-with-lease origin dev",
            "git push --force-with-lease=dev:abc123 origin dev",
            "git push -uf origin dev",
            "git -C /repo push --force origin dev",
            "sudo git push --force origin dev",
            "GIT_TRACE=1 git push --force origin dev",
            "ls && git push --force origin dev",
            "git fetch origin; git push -f origin dev",
        ):
            with self.subTest(command=command):
                self.assertTrue(is_force_push(command))

    def test_allows_ordinary_pushes(self):
        for command in (
            "git push",
            "git push -u origin dev",
            "git push --set-upstream origin claude/continued-session-odztpb",
            "git push origin feature/force-refresh",
            "git push -u origin dev --dry-run",
        ):
            with self.subTest(command=command):
                self.assertFalse(is_force_push(command))

    def test_does_not_block_unrelated_f_options_after_a_push(self):
        """The regression that made git unreachable: 'push' plus a later -f."""
        for command in (
            "git push -u origin dev; grep -f patterns.txt build.log",
            "git push -u origin dev && rm -f /tmp/scratch",
            "git push -u origin dev && tar -x -f dist.tar",
            "git push -u origin dev | tee -f out.log",
            "git log --oneline | grep push | head -n 3",
            "docker compose -f docker-compose.yml up && git push -u origin dev",
        ):
            with self.subTest(command=command):
                self.assertFalse(is_force_push(command))

    def test_ignores_force_flags_belonging_to_other_git_subcommands(self):
        for command in (
            'git commit -m "docs: describe push --force policy"',
            "git checkout -f dev",
            "git clean -f -d",
            "git branch -f dev origin/dev",
        ):
            with self.subTest(command=command):
                self.assertFalse(is_force_push(command))

    def test_survives_unbalanced_quotes(self):
        self.assertTrue(is_force_push('git push --force origin dev "'))
        self.assertFalse(is_force_push('echo "unterminated'))


if __name__ == "__main__":
    unittest.main()
