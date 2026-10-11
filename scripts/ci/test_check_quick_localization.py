import tempfile
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

from scripts.ci.check_quick_localization import VERDICTS, catalog_errors


class CatalogLocalizationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "translations").mkdir()
        self.tree = ET.Element("TS")
        self.context = ET.SubElement(self.tree, "context")
        ET.SubElement(self.context, "name").text = "LoopGlossary"
        for source in sorted(VERDICTS):
            self.message(source, "translated " + source)

    def message(self, source, translation, unfinished=False):
        message = ET.SubElement(self.context, "message")
        ET.SubElement(message, "source").text = source
        node = ET.SubElement(message, "translation")
        node.text = translation
        if unfinished:
            node.set("type", "unfinished")
        return message

    def errors(self, sources=VERDICTS):
        ET.ElementTree(self.tree).write(self.root / "translations/LOOP_es.ts", encoding="utf-8")
        return catalog_errors(self.root, "es", sources)

    def test_missing_shared_verdict_fails(self):
        for message in self.context.findall("message"):
            if message.findtext("source") == "Fail":
                self.context.remove(message)
        self.assertTrue(any("untranslated verdict Fail" in error for error in self.errors()))

    def test_unfinished_verdict_fails(self):
        self.context.find("message/translation").set("type", "unfinished")
        self.assertTrue(any("untranslated verdict" in error for error in self.errors()))

    def test_dropped_diagnostic_placeholder_fails(self):
        self.message("Could not open %1: %2", "No se pudo abrir %1")
        self.assertTrue(any("placeholder mismatch" in error for error in self.errors()))

    def test_same_spelling_and_missing_nonverdict_translation_are_valid(self):
        for message in self.context.findall("message"):
            if message.findtext("source") == "Error":
                message.find("translation").text = "Error"
        self.message("Image resolution", "", unfinished=True)
        self.assertEqual(self.errors(VERDICTS | {"Image resolution"}), [])


if __name__ == "__main__":
    unittest.main()
