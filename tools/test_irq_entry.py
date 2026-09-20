"""Execute the compiled ARM entry, not a C-level imitation of exception entry.

Requires arm-vita-eabi-gcc/nm and optional Unicorn 2.x. Skipped when absent;
run with a Python environment containing Unicorn for the full check. This is
instruction-level validation, NOT emulation of Sony's IRQ dispatcher or GIC.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

try:
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn.arm_const import (
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
        UC_ARM_REG_CPSR, UC_ARM_REG_SPSR, UC_CPU_ARM_CORTEX_A9,
    )
except ImportError:
    Uc = None

ROOT = Path(__file__).resolve().parents[1]
GCC = shutil.which("arm-vita-eabi-gcc")
NM = shutil.which("arm-vita-eabi-nm")
if not GCC and Path("/usr/local/vitasdk/bin/arm-vita-eabi-gcc").is_file():
    GCC = "/usr/local/vitasdk/bin/arm-vita-eabi-gcc"
    NM = "/usr/local/vitasdk/bin/arm-vita-eabi-nm"


@unittest.skipUnless(Uc and GCC and NM, "ARM entry execution requires VitaSDK and Unicorn")
class IrqEntryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="vita-irq-entry-")
        cls.addClassCleanup(cls.tmp.cleanup)
        directory = Path(cls.tmp.name)
        (directory / "observer.c").write_text(
            'void vita_tracy_irq_handler_c(const void *frame) { '
            '__asm__ volatile("" :: "r"(frame) : "memory"); }\n', encoding="ascii")
        cls.images = {}
        source = Path(os.environ.get("VITA_TRACY_IRQ_ENTRY_SOURCE", ROOT / "kernel/irq_entry.S"))
        for thumb in (False, True):
            elf = directory / ("thumb.elf" if thumb else "arm.elf")
            subprocess.run([
                GCC, "-mcpu=cortex-a9", "-mthumb" if thumb else "-marm",
                "-mgeneral-regs-only", "-O2", "-nostdlib", "-I", str(ROOT / "kernel"),
                "-Wl,-Ttext=0x100000", "-Wl,-e,vita_tracy_irq_handler_node",
                str(source), str(directory / "observer.c"), "-o", str(elf),
            ], check=True, capture_output=True, text=True)
            names = subprocess.run([NM, "--defined-only", str(elf)], check=True,
                                   capture_output=True, text=True).stdout
            symbols = {name: int(address, 16) for address, kind, name in
                       (line.split() for line in names.splitlines() if len(line.split()) == 3)}
            blob = elf.read_bytes()
            phoff = struct.unpack_from("<I", blob, 28)[0]
            phsize, phcount = struct.unpack_from("<HH", blob, 42)
            segments = []
            for i in range(phcount):
                kind, offset, addr, _, filesz, memsz, _, _ = struct.unpack_from(
                    "<8I", blob, phoff + i * phsize)
                if kind == 1 and memsz:
                    segments.append((addr, memsz, blob[offset:offset + filesz]))
            cls.images[thumb] = (symbols, segments)

    def exercise(self, thumb=False, stack_skew=0, user_thumb=False, origin_mode=0x10):
        symbols, segments = self.images[thumb]
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
        pages = set()
        for addr, size, data in segments:
            pages.update(range(addr & ~4095, (addr + size + 4095) & ~4095, 4096))
        for addr in sorted(pages):
            uc.mem_map(addr, 4096)
        for addr, size, data in segments:
            uc.mem_write(addr, data)
        stack_base, chain_node = 0x400000, 0x600000
        uc.mem_map(stack_base, 0x10000)
        uc.mem_map(chain_node, 4096)
        uc.mem_write(symbols["vita_tracy_irq_handler_node"], struct.pack("<I", chain_node))
        uc.mem_write(chain_node + 8, b"\x00\x00\xa0\xe1")  # NOP; stopped at its entry.

        user_sp, user_lr = 0x400800, 0x12345679
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F)  # System bank shares user SP/LR.
        uc.reg_write(UC_ARM_REG_SP, user_sp)
        uc.reg_write(UC_ARM_REG_LR, user_lr)
        entry_cpsr = 0xA80A00D2  # IRQ mode, masks, NZCVQ and GE.
        saved_cpsr = 0x60000000 | origin_mode | (0x20 if user_thumb else 0)
        uc.reg_write(UC_ARM_REG_CPSR, entry_cpsr)
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                UC_ARM_REG_R12]
        values = [0] + [0xA0B00000 + i * 17 for i in range(1, 13)]
        for reg, value in zip(regs, values):
            uc.reg_write(reg, value)
        irq_sp, irq_lr = stack_base + 0xF000 + stack_skew, 0x8123456A if user_thumb else 0x81234568
        uc.reg_write(UC_ARM_REG_SP, irq_sp)
        uc.reg_write(UC_ARM_REG_LR, irq_lr)
        uc.reg_write(UC_ARM_REG_SPSR, saved_cpsr)
        seen = []

        def hook(cpu, address, size, _):
            if address == (symbols["vita_tracy_irq_handler_c"] & ~1):
                frame = cpu.reg_read(UC_ARM_REG_R0)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_SP) & 7, 0, "unaligned C call")
                words = struct.unpack("<20I", bytes(cpu.mem_read(frame, 80)))
                self.assertEqual(list(words[:13]), values)
                self.assertEqual(words[13:18], (user_sp, user_lr, irq_lr, saved_cpsr, entry_cpsr))
                seen.append("C")
                return_pc = cpu.reg_read(UC_ARM_REG_LR)
                for reg in regs[:4] + regs[12:]:
                    cpu.reg_write(reg, 0xBAD00001)
                cpu.reg_write(UC_ARM_REG_SPSR, 0x1F)
                current = cpu.reg_read(UC_ARM_REG_CPSR)
                cpu.reg_write(UC_ARM_REG_CPSR, (current & ~0xF80F0020) | ((return_pc & 1) << 5))
                cpu.reg_write(UC_ARM_REG_PC, return_pc & ~1)
            elif address == chain_node + 8:
                self.assertEqual([cpu.reg_read(r) for r in regs], values)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_SP), irq_sp)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_LR), irq_lr)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_SPSR), saved_cpsr)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_CPSR), entry_cpsr)
                cpu.reg_write(UC_ARM_REG_CPSR, 0x1F)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_SP), user_sp)
                self.assertEqual(cpu.reg_read(UC_ARM_REG_LR), user_lr)
                seen.append("chain")
                cpu.emu_stop()

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(symbols["vita_tracy_irq_handler_node"] + 8, 0, count=200)
        self.assertEqual(seen, ["C", "chain"])

    def test_arm_callback_preserves_raw_registers_and_chains(self):
        self.exercise()

    def test_thumb_callback_interworks_and_chains(self):
        self.exercise(thumb=True)

    def test_thumb_interrupted_state_has_distinct_user_lr_and_irq_lr(self):
        self.exercise(user_thumb=True)

    def test_four_byte_aligned_irq_stack_is_aligned_for_c_then_restored(self):
        self.exercise(thumb=True, stack_skew=4, user_thumb=True)

    def test_kernel_interruption_is_forwarded_without_an_exception_return(self):
        self.exercise(origin_mode=0x13)


if __name__ == "__main__":
    unittest.main()
