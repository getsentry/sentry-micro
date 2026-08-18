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
PF_X = 1  # segment is executable


def load_range(path: Path):
    """(lowest PT_LOAD vaddr, bytes from there to the end of the last executable segment).

    The size deliberately ignores non-executable segments above the code. On ESP32 the
    segments are not one contiguous image: they land in four unrelated windows of the
    address map (DROM at 0x3f4.., DRAM at 0x3ff.., IRAM/IROM at 0x40.., and a handful of
    bytes of RTC memory at 0x50000200). Measuring to the end of the *last* loadable
    segment therefore claims ~281 MB, nearly all of it address space this firmware does
    not occupy, and invites Sentry to attribute a stray address to this image and resolve
    it to a confidently wrong symbol.

    Executable segments are the only ones a backtrace PC can point into, so stopping at
    the end of the last one covers every address that can legitimately appear in a frame.
    It is still a wide range — 0x3f400020 to 0x401752b7 spans the gap between DROM and
    IROM — but every byte of code is inside it, which is the property that matters.

    `image_addr` stays the lowest PT_LOAD vaddr regardless: symbolic normalises the
    symbols in the debug file against exactly that value, so lifting it to the first
    executable segment would offset every frame by ~13 MB and resolve all of them wrong.
    """
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
        type_i, vaddr_i, memsz_i, flags_i = 0, 3, 6, 1
    else:
        e_phoff, = struct.unpack_from(endian + "I", data, 0x1C)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x2A)
        fields = endian + "IIIIIIII"  # type, offset, vaddr, paddr, filesz, memsz, flags, align
        type_i, vaddr_i, memsz_i, flags_i = 0, 2, 5, 6

    lowest, highest_exec = None, None
    for i in range(e_phnum):
        entry = struct.unpack_from(fields, data, e_phoff + i * e_phentsize)
        if entry[type_i] != PT_LOAD or entry[memsz_i] == 0:
            continue
        vaddr, memsz = entry[vaddr_i], entry[memsz_i]
        lowest = vaddr if lowest is None else min(lowest, vaddr)
        if entry[flags_i] & PF_X:
            end = vaddr + memsz
            highest_exec = end if highest_exec is None else max(highest_exec, end)

    if lowest is None:
        raise ValueError(f"{path} has no loadable segments")
    if highest_exec is None:
        # No executable segment means no address in a backtrace could belong to this
        # image. Better to fail loudly than to upload debug files that can never match.
        raise ValueError(f"{path} has no executable segments")
    return lowest, highest_exec - lowest


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
        print(f"  image_size 0x{size:x} ({size / 1024 / 1024:.1f} MiB, to the end of the last executable segment)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
