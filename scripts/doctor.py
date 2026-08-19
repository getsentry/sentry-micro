#!/usr/bin/env python3
"""
Check that a release will actually produce a useful issue — before something crashes.

Every step of the release chain reports success whether or not it worked, and the ones that
matter most fail furthest from the point of the mistake. Debug files upload fine and match
nothing. A code mapping that is one directory off produces frames that resolve and a
"suspect commit" section that is simply absent. Nothing anywhere says why, and you find out
weeks later from a device you no longer have.

`release.sh` already refuses to ship a build whose *own* pieces disagree. This asks the
next question out: does the Sentry project agree with the firmware?

    scripts/doctor.py --elf firmware.elf --release 'my-firmware@1.2.3'

Checks, in the order they break:

  * debug files uploaded, and indexed under the id this firmware reports
  * the release exists, is finalized, and carries commits
  * a code mapping whose stack-trace root actually prefixes this ELF's build paths
  * stack trace rules that stop toolchain frames counting as your code

Read-only. It needs a token with `project:read` and `org:read` — the upload-only token a
release uses is not enough, and this says so rather than reporting a missing setting.
"""

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

OK, BAD, UNKNOWN = "  ok  ", " FAIL ", "  ??  "


class Sentry:
    def __init__(self, token, base):
        self.token = token
        self.base = base.rstrip("/")

    def get(self, path, **params):
        """Returns (payload, error). A 403 is an error about *us*, not about the project."""
        url = f"{self.base}{path}"
        if params:
            url += "?" + urllib.parse.urlencode(params)
        request = urllib.request.Request(url, headers={"Authorization": f"Bearer {self.token}"})
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.load(response), None
        except urllib.error.HTTPError as error:
            if error.code in (401, 403):
                return None, "the token cannot read this (needs project:read and org:read)"
            if error.code == 404:
                return None, "not found"
            return None, f"HTTP {error.code}"
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            return None, str(error)


def report(status, title, detail=""):
    print(f"[{status}] {title}")
    if detail:
        for line in detail.splitlines():
            print(f"         {line}")


def debug_id_of(elf, stamp_script):
    """Ask the stamping script, so this and the upload cannot disagree about the id."""
    try:
        out = subprocess.run(["sentry-cli", "debug-files", "check", "--json", str(elf)],
                             capture_output=True, text=True, check=True)
        variants = json.loads(out.stdout).get("variants") or []
        return variants[0].get("debug_id") if variants else None
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, IndexError):
        return None


def build_prefixes(elf, readelf):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from sentry_config import compilation_dirs, find_readelf  # noqa: E402
    return compilation_dirs(elf, find_readelf(readelf))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--release", help="release identifier to check")
    parser.add_argument("--org", default=os.environ.get("SENTRY_ORG"))
    parser.add_argument("--project", default=os.environ.get("SENTRY_PROJECT"))
    parser.add_argument("--project-dir", type=Path, default=Path.cwd())
    parser.add_argument("--readelf")
    parser.add_argument("--url", default=os.environ.get("SENTRY_URL", "https://sentry.io"),
                        help="base API host, e.g. https://us.sentry.io")
    args = parser.parse_args()

    token = os.environ.get("SENTRY_AUTH_TOKEN")
    if not token:
        sys.exit("error: SENTRY_AUTH_TOKEN is not set. This check is read-only, but it needs\n"
                 "       a token with project:read and org:read.")
    if not args.org or not args.project:
        sys.exit("error: pass --org and --project (or set SENTRY_ORG / SENTRY_PROJECT).")
    if not args.elf.is_file():
        sys.exit(f"error: no such ELF: {args.elf}")

    sentry = Sentry(token, args.url + "/api/0")
    failures = 0

    print(f"── {args.org}/{args.project} ─────────────────────────────────")

    # 1. Debug files. Everything downstream is cosmetic if a frame cannot resolve at all.
    debug_id = debug_id_of(args.elf, None)
    if not debug_id:
        report(BAD, "this ELF carries no debug id",
               "It was never stamped, or it was stripped. See scripts/stamp_build_id.py.")
        failures += 1
    else:
        files, error = sentry.get(f"/projects/{args.org}/{args.project}/files/dsyms/",
                                  debug_id=debug_id)
        if error:
            report(UNKNOWN, f"debug files for {debug_id}", error)
        elif files:
            report(OK, f"debug files uploaded for {debug_id}")
        else:
            report(BAD, f"no debug files for {debug_id}",
                   "Every frame will render as a raw address. Run scripts/release.sh.")
            failures += 1

    # 2. The release: exists, finalized, has commits. Commits are what suspect commits reads.
    if args.release:
        encoded = urllib.parse.quote(args.release, safe="")
        release, error = sentry.get(f"/organizations/{args.org}/releases/{encoded}/")
        if error:
            report(UNKNOWN if error != "not found" else BAD, f"release {args.release}",
                   error if error != "not found" else "Not registered. Run scripts/release.sh.")
            failures += error == "not found"
        else:
            report(OK, f"release {args.release} exists")
            if release.get("dateReleased"):
                report(OK, "release is finalized")
            else:
                report(BAD, "release is not finalized",
                       "It has no ship date, so the release comparison views cannot sort it.")
                failures += 1
            count = release.get("commitCount", 0)
            if count:
                report(OK, f"release carries {count} commit(s)")
            else:
                report(BAD, "release carries no commits",
                       "Suspect commits has nothing to choose from. A shallow CI checkout is\n"
                       "the usual cause; see 'fetch-depth: 0'.")
                failures += 1

    # 3. Code mappings, checked against this ELF's real build paths rather than for mere
    #    existence — a mapping that is one directory off looks configured and matches nothing.
    prefixes = build_prefixes(args.elf, args.readelf)
    mappings, error = sentry.get(f"/organizations/{args.org}/code-mappings/",
                                 project=args.project)
    if error:
        report(UNKNOWN, "code mappings", error)
    else:
        roots = [m.get("stackRoot", "") for m in (mappings or [])]
        matched = [r for r in roots if r and any(d.startswith(r.rstrip("/")) for d in prefixes)]
        if matched:
            report(OK, f"code mapping matches this build ({matched[0]})")
        elif roots:
            report(BAD, "code mappings exist but none matches this build",
                   "configured: " + ", ".join(roots) + "\n"
                   "this ELF was built under: " + ", ".join(sorted(prefixes)[:2]) + "\n"
                   "Suspect commits cannot map a frame to a file. "
                   "Run scripts/sentry_config.py.")
            failures += 1
        else:
            report(BAD, "no code mappings",
                   "Sentry does not create these automatically for native projects.\n"
                   "Run scripts/sentry_config.py for the values.")
            failures += 1

    # 4. Stack trace rules. Without them a newlib frame counts as your code, and suspect
    #    commits blames the first in-app frame it sees.
    settings, error = sentry.get(f"/projects/{args.org}/{args.project}/")
    if error:
        report(UNKNOWN, "stack trace rules", error)
    else:
        rules = settings.get("groupingEnhancements") or ""
        if "-app" in rules:
            report(OK, "stack trace rules mark non-app frames")
        else:
            report(BAD, "no stack trace rules",
                   "Toolchain and Arduino frames count as your code, and suspect commits\n"
                   "blames the first in-app frame. Run scripts/sentry_config.py.")
            failures += 1

    print()
    print("nothing to fix" if not failures else f"{failures} thing(s) to fix")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
