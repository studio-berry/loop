#!/usr/bin/env python3
"""Prove the retired milestone command cannot contact or mutate GitHub."""

from __future__ import annotations

import contextlib
import io
import subprocess
import unittest
from unittest import mock

from scripts.github import sync_milestones


class SyncMilestonesTests(unittest.TestCase):
    @mock.patch.object(subprocess, "run", side_effect=AssertionError("external command"))
    def test_dry_run_is_offline_and_plans_no_writes(self, run: mock.Mock) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(sync_milestones.main([]), 0)
        self.assertIn("No GitHub changes are planned", output.getvalue())
        run.assert_not_called()

    @mock.patch.object(subprocess, "run", side_effect=AssertionError("external command"))
    def test_apply_is_refused_before_any_external_command(self, run: mock.Mock) -> None:
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as raised:
            sync_milestones.main(["--apply"])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("synchronization is retired", error.getvalue())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
