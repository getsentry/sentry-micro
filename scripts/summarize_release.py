#!/usr/bin/env python3
"""
Collect the per-variant summaries from a release run into one table.

`release.sh --json-summary` writes one file per build variant. This merges them, prints a
JSON array for a GitHub Action output, and — given `--github-summary` — renders the table
that appears on the run page.

The table is the point. A release that covers four variants and a release that covers
three look identical in a scrolling build log, and the difference only shows up weeks
later as `<unknown>` frames from whichever board nobody uploaded.
"""

import argparse
import json
import sys
from pathlib import Path


def render_table(variants) -> str:
    lines = [
        "### Sentry debug files",
        "",
        "| Environment | Release | Debug ID | Image | Uploaded |",
        "| --- | --- | --- | --- | --- |",
    ]
    for variant in variants:
        size = int(variant.get("image_size", "0"), 0)
        lines.append(
            f"| `{variant['environment']}` "
            f"| `{variant['release']}` "
            f"| `{variant['debug_id']}` "
            f"| `{variant['image_addr']}` +{size / 1024 / 1024:.1f} MiB "
            f"| {'yes' if variant.get('uploaded') else 'no (build only)'} |"
        )
    lines += [
        "",
        f"{len(variants)} variant(s). Each has its own debug ID; an event resolves only "
        "against the one built from the same source at the same release.",
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("directory", type=Path, help="directory of --json-summary files")
    parser.add_argument("--github-summary", type=Path,
                        help="append a Markdown table to this file")
    args = parser.parse_args()

    variants = [json.loads(path.read_text())
                for path in sorted(args.directory.glob("*.json"))]
    if not variants:
        print(f"error: no build summaries in {args.directory}; nothing was released",
              file=sys.stderr)
        return 1

    # Two variants sharing a debug id means Sentry would resolve one binary's addresses
    # against the other and print confidently wrong function names — worse than no
    # symbolication at all, because nothing about the result looks suspect.
    by_debug_id = {}
    for variant in variants:
        by_debug_id.setdefault(variant["debug_id"], []).append(variant["environment"])
    collisions = {k: v for k, v in by_debug_id.items() if len(v) > 1}
    if collisions:
        for debug_id, environments in collisions.items():
            print(f"error: {' and '.join(environments)} share debug_id {debug_id}",
                  file=sys.stderr)
        print("       Frames would resolve against whichever was uploaded last.",
              file=sys.stderr)
        return 1

    if args.github_summary:
        with open(args.github_summary, "a") as handle:
            handle.write(render_table(variants))

    print(json.dumps(variants))
    return 0


if __name__ == "__main__":
    sys.exit(main())
