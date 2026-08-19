"""
PlatformIO pre-script: record source paths relative to the repository.

By default the compiler bakes the *build machine's* absolute paths into DWARF, so a frame
in Sentry resolves to `/Users/someone/code/thing/src/main.cpp`. That is wrong in two ways.

It breaks suspect commits. Sentry works out which commit probably caused an issue by
matching the files in the stack trace against the files changed in the release's commits,
and it cannot match an absolute build path to a path in the repository. The usual fix is a
code mapping configured per project — but the prefix differs between a laptop
(`/Users/...`) and CI (`/home/runner/work/...`), so it needs one mapping per build
environment and a locally built release never matches at all.

It also puts the builder's home directory into every uploaded debug file, and into the
source bundle when `--include-sources` is on. For a public firmware that is somebody's
username on the internet for no reason.

`-ffile-prefix-map` rewrites the prefix at compile time, so the recorded path is
`examples/wifi_basic/src/main.cpp` whoever built it. Nothing to configure in Sentry and
nothing that drifts.

    extra_scripts = pre:path/to/debug_paths.py

**It costs `--include-sources`, so it is opt-in and off by default.** `sentry-cli` finds
source files by reading the paths out of the debug info, and a placeholder prefix does not
exist on disk — measured on this repository, source resolution went from
"Resolved source code for 1 debug information file" to 0. The frames still symbolicate to
function, file and line; what is lost is the line of code shown beside each frame.

So there are two ways to get suspect commits working, and the cheaper one is usually right:

1. **Leave paths absolute and add one code mapping** in Sentry, from the build prefix to the
   repository root. In CI the prefix is stable for a given repository
   (`/home/runner/work/<repo>/<repo>/`), so one mapping covers every release built there.
   Keeps source context. Does not cover locally built releases, and leaves the builder's
   home directory in the debug files.
2. **This script**, plus one code mapping from `/<repo-name>/`. Machine independent, so a
   locally built release matches too, and nothing leaks. Loses source context.

Use 1 unless you have a specific reason not to.

Requires GCC 8 or newer, which every ESP32 toolchain PlatformIO ships satisfies.
"""

import os
import subprocess

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821


def git_toplevel(path):
    """The repository `path` lives in, or None when it is not in one."""
    try:
        result = subprocess.run(["git", "-C", path, "rev-parse", "--show-toplevel"],
                                capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


maps = []

# The repository root, rewritten to a rooted placeholder named after the repository —
# `/sentry-micro/examples/wifi_basic/src/main.cpp`.
#
# Rooted, not empty, and this is the whole subtlety. Mapping the root to "" makes every path
# relative, and DWARF joins a relative `DW_AT_name` onto the compilation unit's `comp_dir`.
# Sources *inside* the project survive that, but anything compiled from outside it — a
# library, a sibling directory — gets silently relocated under the project: this repository's
# own `src/sentry_micro.c` came out as `examples/wifi_basic/src/sentry_micro.c`, a path that
# does not exist and reads exactly like one that does. A rooted prefix is never joined, so
# every path stays the one the file actually has.
#
# The cost is one code mapping in Sentry, from `/<repo-name>/` to the repository root. That
# is one mapping, identical on every machine and in CI, rather than one per build
# environment — which is what absolute build paths would need.
repo_root = git_toplevel(project_dir)
if repo_root:
    maps.append((repo_root + os.sep, "/" + os.path.basename(repo_root) + "/"))

# Toolchain and framework sources are not in anybody's repository, so they cannot be made
# relative to it — but they should still not carry a home directory. Rewritten to a
# recognisable placeholder instead, which also makes it obvious at a glance which frames
# are yours and which are the platform's.
#
# The placeholder is plain text on purpose. These flags reach the compiler through a shell,
# so anything with `<`, `>`, or a glob character in it is interpreted rather than passed —
# an earlier `<platformio>` turned every compile into a shell redirect and failed the build
# with "sh: platformio: No such file or directory".
core_dir = env.subst("$PROJECT_CORE_DIR")  # noqa: F821
if core_dir:
    maps.append((core_dir + os.sep, "/platformio/"))

if maps:
    flags = ["-ffile-prefix-map=%s=%s" % (old, new) for old, new in maps]
    # CCFLAGS covers C and C++; assembly sources carry line info too.
    env.Append(CCFLAGS=flags, ASFLAGS=flags)  # noqa: F821
    for old, new in maps:
        print("debug_paths: %s -> %s" % (old, new or "(repository root)"))
else:
    print("debug_paths: not a git repository; leaving absolute paths in the debug info")
