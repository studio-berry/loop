import importlib.util
import unittest
from pathlib import Path
from types import SimpleNamespace


SPEC = importlib.util.spec_from_file_location('atspi_probe', Path(__file__).resolve().parents[2] /
                                           'tools/ProductQuickAccessibilitySmoke/inspect-linux-atspi.py')
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class NativeNode:
    def __init__(self):
        self.description = 'Fidelity unavailable, origin unknown.'
        self.process_id = 123
        self.states = {'focusable', 'enabled'}
        self.focus_allowed = True

    def clear_cache(self):
        pass

    def get_process_id(self):
        return self.process_id

    def get_description(self):
        return self.description

    def get_name(self):
        return 'Page render fidelity and origin'

    def get_role_name(self):
        return 'label'

    def get_state_set(self):
        return SimpleNamespace(contains=self.states.__contains__)

    def get_component_iface(self):
        return self

    def grab_focus(self):
        if self.focus_allowed:
            self.states.add('focused')
        return self.focus_allowed


class NativeObservationTests(unittest.TestCase):
    def setUp(self):
        self.node = NativeNode()
        self.expected = dict(name=self.node.get_name(), description=self.node.description, focusable=True)
        self.api = SimpleNamespace(StateType=SimpleNamespace(FOCUSABLE='focusable', FOCUSED='focused', ENABLED='enabled'))
        self.process = SimpleNamespace(pid=123)

    def observe(self):
        return probe.observe(self.node, self.expected, 6, self.process, self.api)

    def test_native_focus_and_description_are_observed(self):
        observation = self.observe()
        self.assertTrue(observation['focused'])
        self.assertEqual(observation['process_id'], self.process.pid)

    def test_other_process_cannot_supply_the_tree(self):
        self.node.process_id = 456
        with self.assertRaisesRegex(RuntimeError, 'another process'):
            self.observe()

    def test_missing_native_description_is_rejected(self):
        self.node.description = ''
        with self.assertRaisesRegex(RuntimeError, 'description differs'):
            self.observe()

    def test_refused_native_focus_is_rejected(self):
        self.node.focus_allowed = False
        with self.assertRaisesRegex(RuntimeError, 'focus refused'):
            self.observe()

    def test_wrong_native_availability_is_rejected(self):
        self.expected.pop('focusable')
        self.expected['enabled'] = False
        with self.assertRaisesRegex(RuntimeError, 'availability differs'):
            self.observe()


if __name__ == '__main__':
    unittest.main()
