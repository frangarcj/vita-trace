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
import os
import re
import struct
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
        if module is not None and module.nid != nid:
            raise ValueError(f"module {name} changed NID; split captures before resolving symbols")
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


@dataclass(frozen=True)
class ElfSegment:
    vaddr: int
    memsz: int
    flags: int


class ElfImage:
    """The PT_LOAD table of a little-endian ARM ELF32, in loader order."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.segments: list[ElfSegment] = []
        with open(path, "rb") as handle:
            size = os.fstat(handle.fileno()).st_size
            header = handle.read(52)
            if len(header) != 52 or header[:7] != b"\x7fELF\x01\x01\x01":
                raise ValueError(f"{path}: expected a little-endian ELF32")
            fields = struct.unpack("<16sHHIIIIIHHHHHH", header)
            if fields[2] != 40 or fields[3] != 1 or fields[8] < 52:
                raise ValueError(f"{path}: expected an ARM ELF header")
            phoff, phentsize, phnum = fields[5], fields[9], fields[10]
            if phentsize < 32 or not 0 < phnum < 65535:
                raise ValueError(f"{path}: unsupported program header table")
            if phoff < 52 or phoff + phnum * phentsize > size:
                raise ValueError(f"{path}: truncated program header table")
            for i in range(phnum):
                handle.seek(phoff + i * phentsize)
                kind, offset, vaddr, _, filesz, memsz, flags, _ = struct.unpack(
                    "<8I", handle.read(32)
                )
                if kind != 1:  # PT_LOAD; do not count NOTE/EXIDX/etc. as segments.
                    continue
                if filesz > memsz or offset + filesz > size or vaddr + memsz > 1 << 32:
                    raise ValueError(f"{path}: invalid PT_LOAD segment")
                self.segments.append(ElfSegment(vaddr, memsz, flags))
        if not self.segments:
            raise ValueError(f"{path}: no loadable segments")

    def address(self, resolution: Resolution) -> int:
        if not 0 <= resolution.segment < len(self.segments):
            raise ValueError(f"{self.path}: no PT_LOAD for runtime segment {resolution.segment}")
        segment = self.segments[resolution.segment]
        if not 0 <= resolution.offset < segment.memsz or not segment.flags & 1:
            raise ValueError(f"{self.path}: PC outside executable PT_LOAD; check the ELF build")
        return segment.vaddr + resolution.offset


def load_elfs(specs: list[str], module_map: ModuleMap) -> dict[str, ElfImage]:
    """Never silently apply one module's symbols to another module."""
    names = {module.name for module in module_map.modules}
    result: dict[str, ElfImage] = {}
    for spec in specs:
        if "=" in spec:
            name, path = spec.split("=", 1)
        elif len(names) == 1:
            name, path = next(iter(names)), spec
        else:
            raise ValueError("multiple modules: specify --elf MODULE=path for each ELF")
        if name not in names or name in result or not path:
            raise ValueError(f"unknown, duplicate or empty ELF mapping: {spec}")
        result[name] = ElfImage(path)
    return result


def resolve_with_addr2line(elf: str, address: int, addr2line: str) -> str | None:
    """addr2line expects an ELF virtual address, not a segment-relative offset."""
    try:
        output = subprocess.run(
            [addr2line, "-f", "-C", "-e", elf, hex(address)],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None

    if output.returncode != 0:
        return None

    parts = output.stdout.strip().splitlines()
    if len(parts) < 2 or parts[0] == "??":
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
    parser.add_argument("--elf", action="append", default=[],
                        help="MODULE=path to the matching ELF; repeat for multiple modules")
    parser.add_argument(
        "--addr2line",
        default="arm-vita-eabi-addr2line",
        help="addr2line binary to use",
    )
    args = parser.parse_args(argv)

    module_map = ModuleMap()
    try:
        with open(args.messages, "r", encoding="utf-8", errors="replace") as handle:
            found = module_map.load_messages(handle)
        elfs = load_elfs(args.elf, module_map)
        pcs = [(raw, int(raw, 0)) for raw in args.pc]
        if any(not 0 <= pc < 1 << 32 for _, pc in pcs):
            raise ValueError("PC must fit in 32 bits")
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    if found == 0:
        print("no module placements found in the capture", file=sys.stderr)
        return 1

    failed = False
    for raw_pc, pc in pcs:
        # LR/function pointers may carry the Thumb-state bit. Keep the raw
        # address in the report, but resolve the instruction address.
        resolution = module_map.lookup(pc & ~1)
        if resolution is None:
            print(f"{raw_pc}: no module covers this address")
            failed = True
            continue

        line = (
            f"{raw_pc}: {resolution.module}+0x{resolution.offset:X} "
            f"(seg {resolution.segment})"
        )
        image = elfs.get(resolution.module)
        if image:
            try:
                symbol = resolve_with_addr2line(image.path, image.address(resolution), args.addr2line)
                line += f" -> {symbol or 'unresolved (check symbols and addr2line)'}"
                failed |= symbol is None
            except ValueError as exc:
                line += f" -> {exc}"
                failed = True
        elif elfs:
            line += " -> no ELF supplied for this module"
            failed = True
        print(line)

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
