#!/usr/bin/env python3
import unittest

from symbol_map import ModuleMap

MESSAGE = (
    "vita-tracy module {name} nid=0x{nid:08X} seg={seg} "
    "vaddr=0x{vaddr:08X} size=0x{size:X}"
)


def message(name="SceLibKernel", nid=0x12345678, seg=0, vaddr=0x81000000, size=0x2000):
    return MESSAGE.format(name=name, nid=nid, seg=seg, vaddr=vaddr, size=size)


class ParsingTest(unittest.TestCase):
    def test_parses_a_module_message(self):
        module_map = ModuleMap()
        self.assertTrue(module_map.add_from_message(message()))
        self.assertEqual([m.name for m in module_map.modules], ["SceLibKernel"])

    def test_ignores_unrelated_lines(self):
        module_map = ModuleMap()
        self.assertFalse(module_map.add_from_message("some other tracy message"))
        self.assertFalse(module_map.add_from_message(""))
        self.assertEqual(module_map.modules, [])

    def test_parses_a_message_embedded_in_a_longer_line(self):
        module_map = ModuleMap()
        line = "12:34:56 [message] " + message()
        self.assertTrue(module_map.add_from_message(line))

    def test_counts_only_module_messages(self):
        module_map = ModuleMap()
        lines = [message(), "noise", message(seg=1, vaddr=0x81002000)]
        self.assertEqual(module_map.load_messages(lines), 2)

    def test_repeated_snapshot_replaces_rather_than_duplicates(self):
        # Snapshots can be requested more than once per session.
        module_map = ModuleMap()
        module_map.add_from_message(message(vaddr=0x81000000))
        module_map.add_from_message(message(vaddr=0x90000000))

        segments = module_map.modules[0].segments
        self.assertEqual(len(segments), 1)
        self.assertEqual(segments[0].vaddr, 0x90000000)


class LookupTest(unittest.TestCase):
    def setUp(self):
        self.map = ModuleMap()
        self.map.add_from_message(message(vaddr=0x81000000, size=0x2000))

    def test_start_of_a_segment_is_offset_zero(self):
        resolution = self.map.lookup(0x81000000)
        self.assertIsNotNone(resolution)
        self.assertEqual(resolution.offset, 0)
        self.assertEqual(resolution.module, "SceLibKernel")

    def test_address_inside_a_segment(self):
        self.assertEqual(self.map.lookup(0x81000100).offset, 0x100)

    def test_last_byte_of_a_segment_is_included(self):
        self.assertEqual(self.map.lookup(0x81001FFF).offset, 0x1FFF)

    def test_one_past_the_end_is_excluded(self):
        self.assertIsNone(self.map.lookup(0x81002000))

    def test_address_below_the_module_is_unresolved(self):
        self.assertIsNone(self.map.lookup(0x80FFFFFF))

    def test_unmapped_address_is_unresolved(self):
        self.assertIsNone(self.map.lookup(0x0))

    def test_picks_the_right_segment(self):
        self.map.add_from_message(message(seg=1, vaddr=0x81002000, size=0x1000))
        resolution = self.map.lookup(0x81002010)
        self.assertEqual(resolution.segment, 1)
        self.assertEqual(resolution.offset, 0x10)

    def test_picks_the_right_module(self):
        self.map.add_from_message(
            message(name="SceLibc", nid=0xAABBCCDD, vaddr=0x90000000, size=0x4000)
        )
        resolution = self.map.lookup(0x90000040)
        self.assertEqual(resolution.module, "SceLibc")
        self.assertEqual(resolution.nid, 0xAABBCCDD)
        self.assertEqual(resolution.offset, 0x40)


class AslrTest(unittest.TestCase):
    def test_same_function_resolves_identically_across_runs(self):
        # The whole point of shipping placements with the capture: two runs
        # load the module at different bases, and the same function has to
        # come out at the same offset.
        function_offset = 0x480

        first = ModuleMap()
        first.add_from_message(message(vaddr=0x81000000, size=0x2000))
        second = ModuleMap()
        second.add_from_message(message(vaddr=0x93450000, size=0x2000))

        a = first.lookup(0x81000000 + function_offset)
        b = second.lookup(0x93450000 + function_offset)

        self.assertEqual(a.offset, function_offset)
        self.assertEqual(b.offset, function_offset)
        self.assertEqual(a.module, b.module)

    def test_an_address_valid_in_one_run_is_not_forced_into_another(self):
        second = ModuleMap()
        second.add_from_message(message(vaddr=0x93450000, size=0x2000))
        self.assertIsNone(second.lookup(0x81000480))


if __name__ == "__main__":
    unittest.main()
