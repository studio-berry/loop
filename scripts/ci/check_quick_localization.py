"""Verify shared Quick vocabulary, catalog coverage and extraction inventory."""

import argparse
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
LANGUAGES = ("en", "cs", "de", "es", "fr", "ko", "ru", "tr", "zh_CN", "zh_TW")
VERDICTS = {"Pass", "Fail", "Incomplete", "Error"}


def catalog_errors(root, language, glossary_sources):
    path = root / "translations" / ("LOOP_" + language + ".ts")
    tree = ET.parse(path)
    glossary = next((context for context in tree.findall("context")
                     if context.findtext("name") == "LoopGlossary"), None)
    if glossary is None:
        return [language + ": missing LoopGlossary"]
    messages = {message.findtext("source"): message for message in glossary.findall("message")}
    errors = [language + ": missing glossary source " + source for source in glossary_sources - messages.keys()]
    for source in VERDICTS:
        message = messages.get(source)
        translation = message.find("translation") if message is not None else None
        if translation is None or not translation.text or translation.get("type") == "unfinished":
            errors.append(language + ": untranslated verdict " + source)
    for context in tree.findall("context"):
        for message in context.findall("message"):
            source = message.findtext("source", "")
            translation = message.find("translation")
            if translation is None or translation.get("type") in ("unfinished", "obsolete", "vanished"):
                continue
            expected = set(re.findall(r"%L?\d+|%n", source))
            texts = [translation.text or ""] + [child.text or "" for child in translation]
            for text in texts:
                if text and set(re.findall(r"%L?\d+|%n", text)) != expected:
                    errors.append(language + ": placeholder mismatch in " + source)
    return errors


def check(root=ROOT, project_json=None):
    header = (root / "LoopLibInteraction/sources/shellglossary.h").read_text(encoding="utf-8")
    glossary_sources = set(re.findall(r'QT_TRANSLATE_NOOP\("LoopGlossary", "([^"]+)"\)', header))
    errors = []
    for language in LANGUAGES:
        errors.extend(catalog_errors(root, language, glossary_sources))
    actions = json.loads((root / "docs/loop-shell-actions.json").read_text(encoding="utf-8"))["actions"]
    expected = {action["id"] for action in actions if action["disposition"] not in ("HIDE", "STOP-SHIPPING")}
    menu = (root / "LoopEditor/qml/MenuModel.qml").read_text(encoding="utf-8")
    actual = set(re.findall(r'case "([^"]+)": return qsTr\(', menu))
    if actual != expected:
        errors.append("Extractable menu labels do not cover the manifest commands")
    if project_json is not None:
        project = json.loads(project_json.read_text(encoding="utf-8"))
        sources = set()

        def collect(value):
            if isinstance(value, dict):
                sources.update(value.get("sources", []))
                for child in value.values():
                    collect(child)
            elif isinstance(value, list):
                for child in value:
                    collect(child)

        collect(project)
        required = list((root / "LoopEditor/qml").glob("*.qml")) + [
            root / "LoopEditor/editorhost.cpp", root / "LoopEditor/operatorhelp.cpp",
            root / "LoopEditor/operatorhelpstrings.h", root / "LoopLibInteraction/sources/shellglossary.h"]
        for path in required:
            if path.as_posix() not in sources:
                errors.append("Not extracted by configured lupdate: " + str(path))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-json", type=Path)
    args = parser.parse_args()
    errors = check(project_json=args.project_json)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Quick localization: ten catalogs, shared verdict/finding glossary, placeholders and menu extraction passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
