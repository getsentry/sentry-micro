#!/usr/bin/env python3
"""
Report the address range an ELF occupies once loaded.

Sentry needs this to symbolicate. It computes `relative = instruction_addr - image_addr`
and looks the result up against symbols normalised by the object's own load address, so
`image_addr` in the event must equal the ELF's load address — the lowest `PT_LOAD` virtual
address. Get it wrong and every frame resolves to `<unknown>`, silently: the event still
arrives, the addresses still look right, and nothing anywhere says why.

`image_size` matters too. Sentry attributes a frame to a module by checking it falls inside
`[image_addr, image_addr + image_size)`; without a size there is nothing to fall inside.

Parses the program headers directly rather than shelling out to `readelf`, so it works for
Xtensa and RISC-V targets without needing the matching toolchain on PATH.

    scripts/elf_info.py firmware.elf              # human-readable
    scripts/elf_info.py firmware.elf --export     # shell exports
"""

import argparse
import struct
import sys
from pathlib import Path

PT_LOAD = 1


def load_range(path: Path):
    """(lowest PT_LOAD vaddr, span in bytes) for a 32- or 64-bit little-endian ELF."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{path} is not an ELF file")

    is_64 = data[4] == 2
    little = data[5] == 1
    endian = "<" if little else ">"

    if is_64:
        e_phoff, = struct.unpack_from(endian + "Q", data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x36)
        fields = endian + "IIQQQQQQ"  # type, flags, offset, vaddr, paddr, filesz, memsz, align
        type_i, vaddr_i, memsz_i = 0, 3, 6
    else:
        e_phoff, = struct.unpack_from(endian + "I", data, 0x1C)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x2A)
        fields = endian + "IIIIIIII"  # type, offset, vaddr, paddr, filesz, memsz, flags, align
        type_i, vaddr_i, memsz_i = 0, 2, 5

    lowest, highest = None, 0
    for i in range(e_phnum):
        entry = struct.unpack_from(fields, data, e_phoff + i * e_phentsize)
        if entry[type_i] != PT_LOAD or entry[memsz_i] == 0:
            continue
        vaddr, memsz = entry[vaddr_i], entry[memsz_i]
        lowest = vaddr if lowest is None else min(lowest, vaddr)
        highest = max(highest, vaddr + memsz)

    if lowest is None:
        raise ValueError(f"{path} has no loadable segments")
    return lowest, highest - lowest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("elf", type=Path)
    parser.add_argument("--export", action="store_true",
                        help="print shell exports instead of a table")
    args = parser.parse_args()

    try:
        addr, size = load_range(args.elf)
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.export:
        print(f'export SENTRY_MICRO_IMAGE_ADDR="0x{addr:x}"')
        print(f'export SENTRY_MICRO_IMAGE_SIZE="0x{size:x}"')
    else:
        print(f"  image_addr 0x{addr:08x}")
        print(f"  image_size 0x{size:x} ({size / 1024 / 1024:.1f} MiB span)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
