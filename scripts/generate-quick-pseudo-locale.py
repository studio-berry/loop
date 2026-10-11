"""Generate an expanded accent pseudo-locale and an extraction inventory."""

import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


PROTECTED = re.compile(r"(%L?[0-9]+|%n|<[^>]+>)")
ACCENTS = str.maketrans("aeiouAEIOU", "áëïôüÁËÏÔÜ")


def expand(source):
    parts = PROTECTED.split(source)
    text = "".join(part if PROTECTED.fullmatch(part) else part.translate(ACCENTS)
                   for part in parts)
    return "⟦" + text + " " + "~" * max(3, len(source) // 3) + "⟧"


def generate(source_path, output_directory):
    tree = ET.parse(source_path)
    root = tree.getroot()
    root.set("language", "en_US")
    inventory = {}
    for context in root.findall("context"):
        name = context.findtext("name")
        messages = context.findall("message")
        inventory[name] = len(messages)
        for message in messages:
            source = message.findtext("source", "")
            translation = message.find("translation")
            if translation is None:
                translation = ET.SubElement(message, "translation")
            translation.clear()
            if message.get("numerus") == "yes":
                for _ in range(2):
                    ET.SubElement(translation, "numerusform").text = expand(source)
            else:
                translation.text = expand(source)
    ET.indent(tree, space="    ")
    tree.write(output_directory / "quick-pseudo.ts", encoding="utf-8", xml_declaration=True)
    report = {"source_catalog": str(source_path), "contexts": inventory,
              "source_strings": sum(inventory.values()), "expansion": "33 percent plus delimiters",
              "protected_tokens": "Qt placeholders and rich-text tags"}
    (output_directory / "extraction.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_catalog", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    output = args.output_directory.resolve()
    if output == repository or repository in output.parents:
        parser.error("Keep generated catalogs, reports, and screenshots outside the repository.")
    output.mkdir(parents=True, exist_ok=True)
    generate(args.source_catalog.resolve(), output)


if __name__ == "__main__":
    main()
