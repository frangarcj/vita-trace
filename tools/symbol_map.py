#!/usr/bin/env python3
"""Resolve sampled PCs to module-relative offsets.

The Vita only reports addresses and where each module happens to be mapped.
Because those bases move between runs, an address is only meaningful once it
has been turned back into a module plus an offset; the ELF on the PC then
supplies the function, file and line via addr2line.

Module placements arrive as messages the kernel bridge emits into the
capture, one per segment:

    vita-tracy module SceLibKernel nid=0x12345678 seg=0 vaddr=0x81000000 size=0x2000
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field

MODULE_MESSAGE_RE = re.compile(
    r"vita-tracy module\s+(?P<name>\S+)\s+"
    r"nid=0x(?P<nid>[0-9A-Fa-f]+)\s+"
    r"seg=(?P<seg>\d+)\s+"
    r"vaddr=0x(?P<vaddr>[0-9A-Fa-f]+)\s+"
    r"size=0x(?P<size>[0-9A-Fa-f]+)"
)


@dataclass(frozen=True)
class Segment:
    index: int
    vaddr: int
    size: int

    def contains(self, pc: int) -> bool:
        # Upper bound is exclusive: vaddr + size is the first byte of
        # whatever comes next, not the last byte of this segment.
        return self.vaddr <= pc < self.vaddr + self.size


@dataclass
class Module:
    name: str
    nid: int
    segments: list[Segment] = field(default_factory=list)


@dataclass(frozen=True)
class Resolution:
    module: str
    nid: int
    segment: int
    offset: int


class ModuleMap:
    """Placements of the modules loaded during one capture."""

    def __init__(self) -> None:
        self._modules: dict[str, Module] = {}

    def add_from_message(self, line: str) -> bool:
        """Records one segment placement. Returns False for unrelated lines."""
        match = MODULE_MESSAGE_RE.search(line)
        if not match:
            return False

        name = match.group("name")
        nid = int(match.group("nid"), 16)
        segment = Segment(
            index=int(match.group("seg")),
            vaddr=int(match.group("vaddr"), 16),
            size=int(match.group("size"), 16),
        )

        module = self._modules.get(name)
        if module is None:
            module = Module(name=name, nid=nid)
            self._modules[name] = module

        # A repeated snapshot of the same segment replaces the old placement
        # rather than stacking a duplicate.
        module.segments = [s for s in module.segments if s.index != segment.index]
        module.segments.append(segment)
        module.segments.sort(key=lambda s: s.index)
        return True

    def load_messages(self, lines) -> int:
        return sum(1 for line in lines if self.add_from_message(line))

    @property
    def modules(self) -> list[Module]:
        return sorted(self._modules.values(), key=lambda m: m.name)

    def lookup(self, pc: int) -> Resolution | None:
        """Maps an absolute PC to the module and offset it belongs to."""
        for module in self._modules.values():
            for segment in module.segments:
                if segment.contains(pc):
                    return Resolution(
                        module=module.name,
                        nid=module.nid,
                        segment=segment.index,
                        offset=pc - segment.vaddr,
                    )
        return None


def resolve_with_addr2line(elf: str, offset: int, addr2line: str) -> str | None:
    """Asks addr2line for function:file:line at a module-relative offset."""
    try:
        output = subprocess.run(
            [addr2line, "-f", "-C", "-e", elf, hex(offset)],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None

    if output.returncode != 0:
        return None

    parts = output.stdout.strip().splitlines()
    if len(parts) < 2:
        return None
    return f"{parts[0]} at {parts[1]}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--messages",
        required=True,
        help="file holding the module messages captured during the session",
    )
    parser.add_argument(
        "--pc",
        action="append",
        default=[],
        help="program counter to resolve, may be repeated",
    )
    parser.add_argument("--elf", help="ELF to resolve offsets against")
    parser.add_argument(
        "--addr2line",
        default="arm-vita-eabi-addr2line",
        help="addr2line binary to use",
    )
    args = parser.parse_args(argv)

    module_map = ModuleMap()
    with open(args.messages, "r", encoding="utf-8", errors="replace") as handle:
        found = module_map.load_messages(handle)

    if found == 0:
        print("no module placements found in the capture", file=sys.stderr)
        return 1

    for raw_pc in args.pc:
        pc = int(raw_pc, 0)
        resolution = module_map.lookup(pc)
        if resolution is None:
            print(f"{raw_pc}: no module covers this address")
            continue

        line = (
            f"{raw_pc}: {resolution.module}+0x{resolution.offset:X} "
            f"(seg {resolution.segment})"
        )
        if args.elf:
            symbol = resolve_with_addr2line(args.elf, resolution.offset, args.addr2line)
            if symbol:
                line += f" -> {symbol}"
        print(line)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
