#!/usr/bin/env python3
"""Tests for the #520 search-budget release gate contract."""

from __future__ import annotations

import unittest

from scripts.ci import check_search_budget_gate as gate


class SearchBudgetGateTests(unittest.TestCase):
    def test_passes_on_current_tree(self):
        self.assertEqual(gate.main(), 0)


if __name__ == "__main__":
    unittest.main()
