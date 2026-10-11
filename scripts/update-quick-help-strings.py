"""Refresh extraction markers for bundled operator help presentation text."""

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIELDS = {"name", "title", "description", "help", "getting_started", "measures",
          "limitations", "condition", "meaning"}
RESOURCES = ("LoopEditor/operator/guide.json", "LoopEditor/operator/samples/manifest.json",
             "docs/generated/preflight-check-catalog.json")


def strings(value, translate=False):
    if isinstance(value, str):
        return {value} if translate else set()
    if isinstance(value, list):
        return set().union(*(strings(child, translate) for child in value))
    if isinstance(value, dict):
        return set().union(*(strings(child, key in FIELDS) for key, child in value.items()))
    return set()


def render():
    texts = set()
    for name in RESOURCES:
        texts.update(strings(json.loads((ROOT / name).read_text(encoding="utf-8"))))
    header = "// Generated presentation strings. Refresh with scripts/update-quick-help-strings.py.\n#pragma once\n\n#include <QCoreApplication>\n\n[[maybe_unused]] inline constexpr const char* QuickOperatorHelpSources[] = {\n"
    header += "".join('    QT_TRANSLATE_NOOP("OperatorHelp", ' + json.dumps(text, ensure_ascii=False) + '),\n'
                      for text in sorted(texts))
    return header + "};\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    path = ROOT / "LoopEditor/operatorhelpstrings.h"
    expected = render()
    if args.check:
        if not path.is_file() or path.read_text(encoding="utf-8") != expected:
            parser.error("Operator help extraction markers need refreshing.")
    else:
        path.write_text(expected, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
