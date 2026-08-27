#!/usr/bin/env python3
"""Negative coverage for scripts/verify-widgets-library-consumer-graph.py."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-widgets-library-consumer-graph.py"
GRAPH_PATH = ROOT / "docs" / "generated" / "widgets-library-consumer-graph.json"


def _load_verifier_module():
    spec = importlib.util.spec_from_file_location("verify_widgets_library_consumer_graph", VERIFIER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VerifyWidgetsLibraryConsumerGraphTest(unittest.TestCase):
    def test_valid_graph_passes(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)

    def test_unknown_consumer_class_fails(self) -> None:
        module = _load_verifier_module()
        original = GRAPH_PATH.read_text(encoding="utf-8")
        try:
            graph = json.loads(original)
            graph["consumers"][0]["consumer_class"] = "unclassified"
            graph["acceptance"]["unknown_product_consumers"] = [graph["consumers"][0]["consumer"]]
            GRAPH_PATH.write_text(json.dumps(graph, indent=2) + "\n", encoding="utf-8")
            with patch.object(module, "_run_generator"):
                with self.assertRaises(module.ContractError) as ctx:
                    module.validate_graph(ROOT)
            self.assertIn("unclassified consumer class", str(ctx.exception))
        finally:
            GRAPH_PATH.write_text(original, encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(unittest.main())
