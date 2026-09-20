import contextlib
import io
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from symbol_map import ElfImage, ModuleMap, Resolution, load_elfs, main, resolve_with_addr2line


def elf_bytes(bases=(0x81000000, 0x82000000)):
    # A NOTE precedes the two LOADs so the test catches confusion between a
    # program-header index and the loader's segment index.
    blob = bytearray(0x400)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIIIIIHHHHHH", blob, 0, ident, 2, 40, 1, bases[0],
                     52, 0, 0, 52, 32, len(bases) + 1, 0, 0, 0)
    struct.pack_into("<8I", blob, 52, 4, 0x100, 0, 0, 0, 0, 0, 4)
    for i, base in enumerate(bases):
        struct.pack_into("<8I", blob, 84 + 32 * i, 1, 0x180 + i * 0x80,
                         base, base, 0x40, 0x80, 5, 4)
    return blob


def message(name, base, segment=0, nid=1, size=0x80):
    return (f"vita-tracy module {name} nid=0x{nid:X} seg={segment} "
            f"vaddr=0x{base:X} size=0x{size:X}\n")


class ElfSymbolsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "test.elf"
        self.path.write_bytes(elf_bytes())

    def test_nonzero_elf_load_address_is_not_a_segment_offset(self):
        image = ElfImage(str(self.path))
        self.assertEqual(image.address(Resolution("app", 1, 0, 0x24)), 0x81000024)

    def test_secondary_segment_uses_its_own_elf_virtual_address(self):
        image = ElfImage(str(self.path))
        self.assertEqual(image.address(Resolution("app", 1, 1, 0x20)), 0x82000020)

    def test_rejects_missing_segment_and_out_of_range_offset(self):
        image = ElfImage(str(self.path))
        for segment, offset in [(2, 0), (-1, 0), (0, -1), (0, 0x80)]:
            with self.subTest(segment=segment, offset=offset), self.assertRaises(ValueError):
                image.address(Resolution("app", 1, segment, offset))

    def test_rejects_data_as_code(self):
        blob = elf_bytes()
        struct.pack_into("<I", blob, 84 + 24, 6)
        self.path.write_bytes(blob)
        with self.assertRaises(ValueError):
            ElfImage(str(self.path)).address(Resolution("app", 1, 0, 0))

    def test_rejects_truncated_and_wrong_architecture_elf(self):
        blobs = [b"", elf_bytes()[:60], bytearray(elf_bytes()), bytearray(elf_bytes())]
        blobs[2][4] = 2
        struct.pack_into("<H", blobs[3], 18, 62)
        for blob in blobs:
            self.path.write_bytes(blob)
            with self.assertRaises(ValueError):
                ElfImage(str(self.path))

    def test_does_not_apply_a_single_elf_to_unrelated_modules(self):
        modules = ModuleMap()
        modules.load_messages([message("app", 0x93000000), message("lib", 0x94000000)])
        with self.assertRaisesRegex(ValueError, "MODULE=path"):
            load_elfs([str(self.path)], modules)
        self.assertEqual(set(load_elfs([f"app={self.path}"], modules)), {"app"})
        with self.assertRaises(ValueError):
            load_elfs([f"missing={self.path}"], modules)

    def test_changed_nid_is_not_silently_mixed_with_old_segments(self):
        modules = ModuleMap()
        modules.add_from_message(message("app", 0x93000000))
        with self.assertRaises(ValueError):
            modules.add_from_message(message("app", 0x94000000, nid=2))

    def test_cli_resolves_thumb_pointer_and_aslr_before_addr2line(self):
        messages = Path(self.tmp.name) / "modules.txt"
        messages.write_text(message("app", 0x93400000))
        with patch("symbol_map.resolve_with_addr2line", return_value="hotspot at test.c:1") as resolve:
            with contextlib.redirect_stdout(io.StringIO()):
                result = main(["--messages", str(messages), "--elf", str(self.path),
                               "--pc", "0x93400025"])
            self.assertEqual(result, 0)
            resolve.assert_called_once_with(str(self.path), 0x81000024, "arm-vita-eabi-addr2line")

    def test_unresolved_addr2line_is_reported_as_failure(self):
        result = subprocess.CompletedProcess([], 0, "??\n??:0\n", "")
        with patch("symbol_map.subprocess.run", return_value=result):
            self.assertIsNone(resolve_with_addr2line(str(self.path), 0x81000000, "addr2line"))

    def test_real_arm_elf_resolves_a_function_after_relocation(self):
        gcc = shutil.which("arm-vita-eabi-gcc")
        nm = shutil.which("arm-vita-eabi-nm")
        addr2line = shutil.which("arm-vita-eabi-addr2line")
        if not all((gcc, nm, addr2line)):
            self.skipTest("VitaSDK toolchain not on PATH")
        source = Path(self.tmp.name) / "probe.c"
        source.write_text("int hotspot(int x) { return x * 3 + 1; }\n")
        subprocess.run([gcc, "-g", "-O0", "-nostdlib", "-Wl,--entry=hotspot",
                        "-Wl,-Ttext=0x81000000", str(source), "-o", str(self.path)],
                       check=True, capture_output=True, timeout=30)
        symbols = subprocess.run([nm, "-n", str(self.path)], check=True,
                                 capture_output=True, text=True, timeout=10).stdout
        address = next(int(line.split()[0], 16) & ~1 for line in symbols.splitlines()
                       if line.split()[-1:] == ["hotspot"])
        image = ElfImage(str(self.path))
        segment = next(i for i, seg in enumerate(image.segments)
                       if seg.vaddr <= address < seg.vaddr + seg.memsz)
        offset = address - image.segments[segment].vaddr
        modules = ModuleMap()
        modules.add_from_message(message("app", 0x93400000, segment,
                                         size=image.segments[segment].memsz))
        resolution = modules.lookup(0x93400000 + offset)
        self.assertIsNotNone(resolution)
        resolved = resolve_with_addr2line(str(self.path), image.address(resolution), addr2line)
        self.assertIsNotNone(resolved)
        self.assertIn("hotspot", resolved)
        self.assertIn("probe.c:1", resolved)


if __name__ == "__main__":
    unittest.main()
