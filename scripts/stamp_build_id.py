#!/usr/bin/env python3
"""
Stamp a GNU build-id into a linked ELF, without disturbing the loaded image.

Why not just pass `-Wl,--build-id` and let the linker do it? Because on ESP32 that is
actively harmful. The linker marks the note ALLOC and places it at the start of IRAM, which
pushes `.iram0.vectors` off `0x40080000`:

    without the flag   .iram0.vectors   0x40080000     (1KB-aligned, correct)
    with the flag      .note...build-id 0x40080000
                       .iram0.vectors   0x40080024     (misaligned)

VECBASE requires 1KB alignment, so the first interrupt jumps into the note. Measured, not
theorised: the board boot-loops with `rst:0x10 (RTCWDT_RTC_RESET)`.

Adding the note afterwards as a **non-ALLOC** section avoids all of that — it lives in the
ELF, where `sentry-cli` and Sentry's symbolication read it, and never reaches flash.

It also removes a chicken-and-egg problem. The firmware has to *know* its own build-id in
order to report it, but a linker-computed one only exists after linking. Choosing the value
ourselves means the same string can be compiled in and stamped on.
"""

import argparse
import hashlib
import struct
import subprocess
import sys
import uuid
from pathlib import Path

NT_GNU_BUILD_ID = 3


def derive_build_id(seed: str) -> str:
    """A deterministic 20-byte id from a seed string, as lowercase hex."""
    return hashlib.sha1(seed.encode("utf-8")).hexdigest()


def debug_id_for(build_id_hex: str) -> str:
    """
    The id Sentry indexes debug files under.

    First 16 bytes of the build-id read as a *little-endian* UUID. Mirrors
    `sentry_debug_id_from_code_id()` in the firmware and `code_id_to_debug_id` in
    getsentry/coredump-uploader; all three must agree or lookups silently miss.
    """
    padded = build_id_hex + "00" * 16
    return str(uuid.UUID(bytes_le=bytes.fromhex(padded)[:16]))


def build_note(build_id_hex: str) -> bytes:
    """An ELF note: namesz, descsz, type, "GNU\\0", then the id."""
    desc = bytes.fromhex(build_id_hex)
    return struct.pack("<III", 4, len(desc), NT_GNU_BUILD_ID) + b"GNU\0" + desc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("elf", type=Path, help="linked ELF to stamp, in place")
    parser.add_argument("--build-id", help="20-byte id as hex; derived from --seed if omitted")
    parser.add_argument("--seed", help="string to derive a deterministic build-id from")
    parser.add_argument("--objcopy", default="xtensa-esp32-elf-objcopy")
    parser.add_argument("--print-debug-id", action="store_true",
                        help="print only the resulting debug id, for scripting")
    args = parser.parse_args()

    if not args.elf.is_file():
        return parser.error(f"no such ELF: {args.elf}")
    if not args.build_id and not args.seed:
        return parser.error("pass --build-id or --seed")

    build_id = args.build_id or derive_build_id(args.seed)
    try:
        raw = bytes.fromhex(build_id)
    except ValueError:
        return parser.error(f"--build-id must be hex: {build_id!r}")
    if len(raw) != 20:
        return parser.error(f"--build-id must be 20 bytes (40 hex chars), got {len(raw)}")

    note = args.elf.with_suffix(".build-id-note")
    note.write_bytes(build_note(build_id))
    try:
        subprocess.run(
            [
                args.objcopy,
                # Replace any existing note so re-running is idempotent.
                "--remove-section", ".note.gnu.build-id",
                "--add-section", f".note.gnu.build-id={note}",
                # readonly+contents and no `alloc`: present in the file, absent from memory.
                "--set-section-flags", ".note.gnu.build-id=readonly,contents",
                str(args.elf),
            ],
            check=True,
        )
    except FileNotFoundError:
        print(f"error: {args.objcopy} not found. Pass --objcopy with the full path;\n"
              "       PlatformIO keeps one under ~/.platformio/packages/toolchain-*/bin/.",
              file=sys.stderr)
        return 1
    finally:
        note.unlink(missing_ok=True)

    debug_id = debug_id_for(build_id)
    if args.print_debug_id:
        print(debug_id)
    else:
        print(f"stamped {args.elf.name}")
        print(f"  code_id  {build_id}")
        print(f"  debug_id {debug_id}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
