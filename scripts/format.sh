#!/usr/bin/env bash
#
# Format (or check) every source file in the repo.
#
#   scripts/format.sh          rewrite files in place
#   scripts/format.sh --check  exit non-zero if anything is unformatted (what CI runs)
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

# Only our own sources: -prune keeps this out of .pio/, which holds the entire Arduino core
# and ESP-IDF once anything has been built.
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

if [ ${#files[@]} -eq 0 ]; then
    echo "no sources found" >&2
    exit 1
fi

if [ "${1:-}" = "--check" ]; then
    "$CLANG_FORMAT" --dry-run -Werror "${files[@]}"
    echo "${#files[@]} files are correctly formatted."
else
    "$CLANG_FORMAT" -i "${files[@]}"
    echo "formatted ${#files[@]} files."
fi
