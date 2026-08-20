#!/usr/bin/env bash
#
# Format (or check) source files — every one in the repo by default, or just the ones
# named on the command line.
#
#   scripts/format.sh                    rewrite every file in place
#   scripts/format.sh --check            exit non-zero if anything is unformatted (what CI runs)
#   scripts/format.sh foo.c bar.h        rewrite only those files
#   scripts/format.sh --check foo.c      check only that file
#
# The explicit-file form exists for the pre-commit hook (.pre-commit-config.yaml): it should
# format only what a commit actually touches, not fail it over some unrelated file elsewhere
# in the tree that CI's whole-repo check would also catch on its own.
#
# clang-format's output shifts between major versions, so a contributor on a different
# version would otherwise "fix" formatting on every file they touch. CI pins 22.1.8 via pip;
# match it locally with `pip install clang-format==22.1.8` or `brew install clang-format`.

set -euo pipefail

cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: $CLANG_FORMAT not found. Install it with 'pip install clang-format==22.1.8'." >&2
    exit 1
fi

CHECK=0
if [ "${1:-}" = "--check" ]; then
    CHECK=1
    shift
fi

if [ "$#" -gt 0 ]; then
    # Explicit files, e.g. from pre-commit: format/check exactly those, in the order given.
    files=("$@")
else
    # Only our own sources: -prune keeps this out of .pio/, which holds the entire Arduino
    # core and ESP-IDF once anything has been built.
    # (A read loop rather than `mapfile`, which needs bash 4 — macOS still ships bash 3.2.)
    files=()
    while IFS= read -r file; do
        files+=("$file")
    done < <(
        find . \
            \( -name .git -o -name .pio -o -name build \) -prune -o \
            \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) -print |
            sort
    )
fi

if [ ${#files[@]} -eq 0 ]; then
    echo "no sources found" >&2
    exit 1
fi

if [ "$CHECK" -eq 1 ]; then
    "$CLANG_FORMAT" --dry-run -Werror "${files[@]}"
    echo "${#files[@]} files are correctly formatted."
else
    "$CLANG_FORMAT" -i "${files[@]}"
    echo "formatted ${#files[@]} files."
fi
