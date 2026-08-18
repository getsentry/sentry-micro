#!/usr/bin/env python3
"""
Confirm a stamped ELF is one Sentry can actually symbolicate against.

Every other step in the release chain reports success whether or not it worked. If the
build-id note failed to land, `objcopy` still exits 0; the upload still succeeds; the
events still arrive. The only symptom is `<unknown>` on every frame, days later, from a
device you no longer have on your desk.

This asks the question directly, using the same reader Sentry runs server-side
(`sentry-cli debug-files check`, which is `symbolic` under the hood):

  * is the file usable at all — does it have a debug identifier and debug information?
  * is that identifier the one the firmware was compiled to report?

The second half is the one that matters for a build matrix. Each variant compiles in its
own build-id, and a mismatch means Sentry has debug files for a binary that is not the one
that crashed.

    scripts/check_debug_file.py firmware.elf --expect-debug-id 12345678-...
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("elf", type=Path)
    parser.add_argument("--expect-debug-id",
                        help="fail unless the ELF carries this debug id")
    parser.add_argument("--sentry-cli", default="sentry-cli",
                        help="path to sentry-cli (default: found on PATH)")
    parser.add_argument("--optional", action="store_true",
                        help="warn instead of failing when sentry-cli is not installed")
    args = parser.parse_args()

    if not args.elf.is_file():
        print(f"error: no such ELF: {args.elf}", file=sys.stderr)
        return 1

    if shutil.which(args.sentry_cli) is None:
        message = (f"{args.sentry_cli} not found, so the debug file was not verified.\n"
                   "       brew install getsentry/tools/sentry-cli, or see "
                   "https://docs.sentry.io/cli/")
        if args.optional:
            print(f"  (skipped: {message})")
            return 0
        print(f"error: {message}", file=sys.stderr)
        return 1

    result = subprocess.run([args.sentry_cli, "debug-files", "check", "--json", str(args.elf)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(f"error: sentry-cli could not read {args.elf.name}:\n{result.stderr.strip()}",
              file=sys.stderr)
        return 1

    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        print(f"error: unexpected output from sentry-cli:\n{result.stdout[:400]}",
              file=sys.stderr)
        return 1

    found = [variant.get("debug_id") for variant in report.get("variants", [])]

    if not report.get("is_usable"):
        # "missing debug identifier, likely stripped" is what an un-stamped ELF reports,
        # and is by far the most common way to get here.
        print(f"error: Sentry cannot use {args.elf.name}: {report.get('problem')}\n"
              "       The build-id note is missing or the file was stripped; see "
              "scripts/stamp_build_id.py.", file=sys.stderr)
        return 1

    if args.expect_debug_id and args.expect_debug_id not in found:
        print(f"error: {args.elf.name} carries debug_id {', '.join(map(str, found))},\n"
              f"       but the firmware reports {args.expect_debug_id}.\n"
              "       Sentry matches on that id, so nothing would resolve. The ELF was\n"
              "       probably stamped with a different build-id than the one compiled in.",
              file=sys.stderr)
        return 1

    variant = (report.get("variants") or [{}])[0]
    print(f"  usable, features: {report.get('features')}")
    print(f"  debug_id {variant.get('debug_id')}  arch {variant.get('arch')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
