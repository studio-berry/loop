#!/usr/bin/env python3
"""Tests for the #241 independent-validation release gate contract."""

from __future__ import annotations

import unittest

from scripts.ci import check_independent_validation_gate as gate


class IndependentValidationGateTests(unittest.TestCase):
    def test_passes_on_current_tree(self):
        self.assertEqual(gate.main(), 0)


if __name__ == "__main__":
    unittest.main()
