#!/usr/bin/env python3
"""
Reconcile the build variants a project declares against the ones a release covers.

This is the part of the release chain that has no other safety net. Every board and
feature variant is a distinct binary with its own build-id, and each one needs its own
debug files uploaded. Miss one and the firmware still works, the events still arrive, and
users of *that variant* get raw hex addresses while everyone else gets frames — with
nothing anywhere reporting the gap. WLED ships dozens of variants; the odds of noticing by
eye are poor.

So the release refuses to run against a partial list. Every `[env:...]` in the
`platformio.ini` must be either built or explicitly named as skipped:

    scripts/list_environments.py examples/wifi_basic          # all of them
    scripts/list_environments.py . --skip esp32-c6            # all but one, on purpose

Adding a board to `platformio.ini` therefore breaks the release until someone decides
whether it ships — which is the correct default, and the opposite of what a hand-written
list of environments does.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ENV_SECTION = re.compile(r"^\s*\[env:([^\]]+)\]")


def declared_environments(ini: Path):
    """Every `[env:NAME]` in the file, in declaration order.

    Deliberately not configparser: PlatformIO's dialect has `;` comments and multi-line
    values that trip it up, and section names are the only thing needed here.
    """
    return [match.group(1).strip()
            for match in map(ENV_SECTION.match, ini.read_text().splitlines())
            if match]


def split_list(value: str):
    """Accept comma- or newline-separated lists, since YAML makes both natural."""
    return [item.strip() for item in re.split(r"[,\n]", value or "") if item.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("project_dir", type=Path,
                        help="PlatformIO project directory, or a platformio.ini")
    parser.add_argument("--select", default="",
                        help="environments to build (default: every one declared)")
    parser.add_argument("--skip", default="",
                        help="environments deliberately not built; required for any "
                             "declared environment left out of --select")
    parser.add_argument("--json", action="store_true", help="print a JSON array")
    args = parser.parse_args()

    ini = args.project_dir if args.project_dir.is_file() else args.project_dir / "platformio.ini"
    if not ini.is_file():
        print(f"error: no platformio.ini at {ini}", file=sys.stderr)
        return 1

    declared = declared_environments(ini)
    if not declared:
        print(f"error: {ini} declares no [env:...] sections", file=sys.stderr)
        return 1

    selected = split_list(args.select) or [name for name in declared
                                           if name not in split_list(args.skip)]
    skipped = split_list(args.skip)

    unknown = [name for name in selected + skipped if name not in declared]
    if unknown:
        print(f"error: not declared in {ini}: {', '.join(unknown)}\n"
              f"       it declares: {', '.join(declared)}", file=sys.stderr)
        return 1

    missed = [name for name in declared if name not in selected and name not in skipped]
    if missed:
        print(f"error: {len(missed)} build variant(s) would ship without debug files: "
              f"{', '.join(missed)}\n"
              "       Devices running them report crashes as raw addresses and nothing\n"
              "       reports the gap. Add them to the environments being built, or name\n"
              "       them as deliberately skipped.", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(selected))
    else:
        print("\n".join(selected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
