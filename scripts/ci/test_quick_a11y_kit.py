import tempfile
import unittest
from pathlib import Path

from scripts.ci import quick_a11y_kit as kit


class AccessibilityKitTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / 'ProductQuickAccessibilitySmoke.exe'
        self.binary.write_bytes(b'qualification probe')
        self.sha = 'a' * 40
        self.kit = self.root / 'kit'

    def test_exact_source_and_binary_pass(self):
        kit.create(self.binary, self.kit, self.sha)
        self.assertEqual(kit.verify(self.kit, self.sha).read_bytes(), self.binary.read_bytes())

    def test_replaced_binary_is_rejected(self):
        kit.create(self.binary, self.kit, self.sha)
        (self.kit / self.binary.name).write_bytes(b'other source binary')
        with self.assertRaisesRegex(ValueError, 'identity'):
            kit.verify(self.kit, self.sha)

    def test_other_source_is_rejected(self):
        kit.create(self.binary, self.kit, self.sha)
        with self.assertRaisesRegex(ValueError, 'identity'):
            kit.verify(self.kit, 'b' * 40)

    def test_short_source_is_rejected_before_writing(self):
        with self.assertRaises(ValueError):
            kit.create(self.binary, self.kit, 'abcd')
        self.assertFalse(self.kit.exists())

    def test_existing_kit_cannot_be_reused(self):
        kit.create(self.binary, self.kit, self.sha)
        with self.assertRaises(FileExistsError):
            kit.create(self.binary, self.kit, self.sha)

    def test_unexpected_executable_is_rejected_before_writing(self):
        other = self.root / 'unrelated.exe'
        other.write_bytes(b'other executable')
        with self.assertRaisesRegex(ValueError, 'unexpected'):
            kit.create(other, self.kit, self.sha)
        self.assertFalse(self.kit.exists())


if __name__ == '__main__':
    unittest.main()
