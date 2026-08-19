#!/usr/bin/env python3
"""
Derive the Sentry project settings this firmware needs, from the firmware itself.

Symbolication works out of the box. The two settings that make an issue *actionable* —
suspect commits, and knowing which frames are yours — do not, and Sentry cannot infer them
for a native project: automatic code mappings cover JavaScript, Python, Java, PHP, Ruby, Go,
C# and Kotlin, and nothing else. So they are set by hand, from values only the build knows,
and getting either wrong fails quietly.

The ELF already holds both. It records the absolute path of every source file that went into
it, so this reads them back and prints the settings that follow:

    scripts/sentry_config.py firmware.elf --project-dir examples/wifi_basic

**Code mapping.** Sentry matches a stack frame to a file in a commit by rewriting the frame's
path, and the compiler records the *build machine's* path. Nothing lines up until a mapping
strips that prefix. Run this on the ELF a release was actually built from — in CI, that is
the only place the prefix is knowable.

**Stack trace rules.** Suspect commits blames the first *in-app* frame, so if a frame from
newlib or the Arduino core counts as yours, it blames a file that is not in your repository
and stops looking. The example firmware here is built from 21 distinct
directories and only one of them is ours — the rest are Espressif's CI, a GitLab runner and
two strangers' home directories, baked into prebuilt libraries. Enumerating those is
hopeless and they change with the toolchain, so the rules invert it: nothing is in-app until
proven otherwise, then the directory that is yours is added back.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def find_readelf(explicit=None):
    """The toolchain's readelf. Xtensa and RISC-V need different ones; PlatformIO has both."""
    if explicit:
        return explicit
    root = Path.home() / ".platformio" / "packages"
    for candidate in sorted(root.glob("toolchain-*/bin/*-elf-readelf")):
        return str(candidate)
    return "readelf"


def compilation_dirs(elf, readelf):
    """Every `DW_AT_comp_dir` in the ELF: the directory each translation unit was built in."""
    try:
        result = subprocess.run([readelf, "--debug-dump=info", str(elf)],
                                capture_output=True, text=True, check=True)
    except FileNotFoundError:
        sys.exit(f"error: {readelf} not found; pass --readelf with a full path")
    except subprocess.CalledProcessError as error:
        sys.exit(f"error: could not read debug info from {elf}:\n{error.stderr[:400]}")

    dirs = set()
    for line in result.stdout.splitlines():
        if "DW_AT_comp_dir" in line:
            # readelf renders these two ways, and an indirect string puts a second ": "
            # in the middle of the line — so take the last field, not the first.
            #   DW_AT_comp_dir : /path
            #   DW_AT_comp_dir : (indirect string, offset: 0x1cd04): /path
            dirs.add(line.rsplit(": ", 1)[-1].strip())
    return dirs


def repo_root(path):
    try:
        out = subprocess.run(["git", "-C", str(path), "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip() or None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("elf", type=Path)
    parser.add_argument("--project-dir", type=Path, default=Path.cwd(),
                        help="the PlatformIO project, used to locate the repository")
    parser.add_argument("--readelf", help="path to the toolchain's readelf")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()

    if not args.elf.is_file():
        sys.exit(f"error: no such ELF: {args.elf}")

    root = repo_root(args.project_dir)
    if not root:
        sys.exit(f"error: {args.project_dir} is not in a git repository, so there is no "
                 "source root to map onto")

    dirs = compilation_dirs(args.elf, find_readelf(args.readelf))
    if not dirs:
        sys.exit(f"error: {args.elf.name} carries no debug info; build with -g and do not "
                 "strip it")

    # String prefixes, not "does this directory exist": this has to give the same answer
    # when run on a machine that did not do the build.
    ours = sorted(d for d in dirs if d == root or d.startswith(root + os.sep))
    theirs = len(dirs) - len(ours)
    if not ours:
        sys.exit(f"error: none of the {len(dirs)} build directories in {args.elf.name} are "
                 f"under {root}.\n"
                 "       Either this ELF was built elsewhere, or --project-dir is wrong.")

    config = {
        "repository_root": root,
        "code_mapping": {"stack_root": root + os.sep, "source_root": ""},
        "stack_trace_rules": [
            "family:native -app",
            f"stack.abs_path:{root}/** +app",
            "stack.abs_path:**/.pio/libdeps/** -app",
        ],
        "app_build_dirs": ours,
        "vendor_build_dir_count": theirs,
    }

    if args.json:
        print(json.dumps(config, indent=2))
        return 0

    print("── Sentry settings for this build ────────────────────────────")
    print()
    print("Code mapping   (Settings -> Integrations -> your repo -> Code Mappings)")
    print(f"  Stack trace root : {config['code_mapping']['stack_root']}")
    print("  Source code root : (leave empty — the repository root)")
    print()
    print("Stack trace rules   (Settings -> Projects -> your project -> Processing)")
    for rule in config["stack_trace_rules"]:
        print(f"  {rule}")
    print()
    print(f"Derived from {len(ours)} build director{'y' if len(ours) == 1 else 'ies'} of your "
          f"own and {theirs} from toolchains and prebuilt libraries.")
    print("The blanket -app is deliberate: suspect commits blames the first in-app frame, so")
    print("a newlib frame counting as yours would blame a file you do not have.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
