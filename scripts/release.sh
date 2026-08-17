#!/usr/bin/env bash
#
# Build firmware and make its backtraces readable in Sentry — in one command.
#
#   scripts/release.sh -e esp32dev
#   scripts/release.sh -e esp32-s3 -r 'my-firmware@1.2.3' --upload-firmware
#
# It runs these steps, which have to agree with each other and are easy to get wrong by
# hand:
#
#   1. Choose a build-id, deterministically, from the release name and the target.
#   2. Build with that id compiled in, so the device reports it on every event.
#   3. Stamp the same id into the ELF as a non-ALLOC note (see stamp_build_id.py for why
#      -Wl,--build-id cannot be used here — it boot-loops the chip).
#   4. Upload the ELF to Sentry, which indexes it under the id derived from that note.
#
# After this, an event whose frames carry raw addresses gets resolved server-side into
# functions, files and line numbers. Skip it and the addresses stay hex forever — the
# firmware still reports crashes, they are just not readable.
#
# Requires: platformio, and sentry-cli for the upload step
# (`brew install getsentry/tools/sentry-cli` or https://docs.sentry.io/cli/).

set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"

ENVIRONMENT="esp32dev"
RELEASE=""
PROJECT_DIR="examples/wifi_basic"
DO_UPLOAD=1
UPLOAD_FIRMWARE=0

usage() {
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  -e, --env ENV          PlatformIO environment (default: esp32dev)
  -r, --release NAME     release identifier, e.g. my-firmware@1.2.3
                         (default: <env>@<git-describe or 'dev'>)
  -d, --project-dir DIR  PlatformIO project (default: examples/wifi_basic)
      --no-upload        build and stamp, but do not talk to Sentry
      --upload-firmware  also flash the result to a connected board
  -h, --help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -e|--env) ENVIRONMENT="$2"; shift 2 ;;
        -r|--release) RELEASE="$2"; shift 2 ;;
        -d|--project-dir) PROJECT_DIR="$2"; shift 2 ;;
        --no-upload) DO_UPLOAD=0; shift ;;
        --upload-firmware) UPLOAD_FIRMWARE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v pio >/dev/null 2>&1 || { echo "error: platformio (pio) not found" >&2; exit 1; }

# Default the release to something traceable back to a commit.
if [ -z "$RELEASE" ]; then
    if git rev-parse --git-dir >/dev/null 2>&1; then
        VERSION="$(git describe --always --dirty 2>/dev/null || echo dev)"
    else
        VERSION="dev"
    fi
    RELEASE="${ENVIRONMENT}@${VERSION}"
fi

# The build-id is derived from the release and the target, so the same source at the same
# release always produces the same id — and two *different* variants never collide. That
# matters: every board in a build matrix is a distinct binary and needs a distinct id, or
# Sentry resolves addresses against the wrong firmware and prints confidently wrong frames.
BUILD_ID="$(python3 -c "
import hashlib, sys
print(hashlib.sha1(sys.argv[1].encode()).hexdigest())
" "${RELEASE}/${ENVIRONMENT}")"

echo "── sentry-micro release ──────────────────────────"
echo "  env       ${ENVIRONMENT}"
echo "  release   ${RELEASE}"
echo "  build-id  ${BUILD_ID}"
echo

# Both compiled in via scripts/env_secrets.py, so the firmware reports exactly the build-id
# we stamp and exactly the release we register. Leaving the release to a constant in the
# firmware means events arrive labelled with a different release than the one that owns the
# debug files — harmless for symbolication, which matches on debug_id, but thoroughly
# confusing in the issue stream.
export SENTRY_MICRO_BUILD_ID="$BUILD_ID"
export SENTRY_MICRO_RELEASE="$RELEASE"

ELF="${REPO_ROOT}/${PROJECT_DIR}/.pio/build/${ENVIRONMENT}/firmware.elf"

# Two passes, and the second one is not optional.
#
# Sentry maps an address back to a function using `instruction_addr - image_addr`, so the
# firmware has to report the ELF's load address and extent — which only exist once the ELF
# has been linked. The first pass produces an ELF to measure; the second bakes the
# measurements in. Skipping it costs nothing visible and silently produces `<unknown>` for
# every frame, which is a much worse failure than a slow build.
echo "→ building (pass 1: measure the image)"
( cd "$PROJECT_DIR" && pio run -e "$ENVIRONMENT" >/dev/null )
[ -f "$ELF" ] || { echo "error: no ELF at $ELF" >&2; exit 1; }

eval "$(python3 "${REPO_ROOT}/scripts/elf_info.py" "$ELF" --export)"
echo "  image_addr ${SENTRY_MICRO_IMAGE_ADDR}"
echo "  image_size ${SENTRY_MICRO_IMAGE_SIZE}"
export SENTRY_MICRO_IMAGE_ADDR SENTRY_MICRO_IMAGE_SIZE

echo
echo "→ building (pass 2: bake it in)"
( cd "$PROJECT_DIR" && pio run -e "$ENVIRONMENT" )

# Adding the -D flags relinks, which can shift the layout. Re-measure and refuse to ship a
# mismatch rather than uploading debug files that do not describe the binary.
eval "$(python3 "${REPO_ROOT}/scripts/elf_info.py" "$ELF" --export | sed 's/SENTRY_MICRO_IMAGE/FINAL_IMAGE/')"
if [ "$FINAL_IMAGE_ADDR" != "$SENTRY_MICRO_IMAGE_ADDR" ] || \
   [ "$FINAL_IMAGE_SIZE" != "$SENTRY_MICRO_IMAGE_SIZE" ]; then
    echo
    echo "error: the image moved between passes." >&2
    echo "       compiled in: ${SENTRY_MICRO_IMAGE_ADDR} +${SENTRY_MICRO_IMAGE_SIZE}" >&2
    echo "       final ELF:   ${FINAL_IMAGE_ADDR} +${FINAL_IMAGE_SIZE}" >&2
    echo "       Re-run; if it persists the build is not reproducible." >&2
    exit 1
fi

# Use the toolchain objcopy that matches the target: an Xtensa ELF and a RISC-V one need
# different binutils, and PlatformIO already installed whichever this env used.
OBJCOPY="$(find "$HOME/.platformio/packages" -type f -name '*-elf-objcopy' 2>/dev/null \
    | grep -E "$( [ "${ENVIRONMENT#esp32-c}" != "$ENVIRONMENT" ] && echo riscv32 || echo xtensa )" \
    | head -1)"
OBJCOPY="${OBJCOPY:-objcopy}"

echo
echo "→ stamping the build-id into the ELF"
python3 "${REPO_ROOT}/scripts/stamp_build_id.py" "$ELF" --build-id "$BUILD_ID" --objcopy "$OBJCOPY"

if [ "$DO_UPLOAD" -eq 1 ]; then
    if ! command -v sentry-cli >/dev/null 2>&1; then
        echo
        echo "error: sentry-cli not found — install it, or pass --no-upload." >&2
        echo "       brew install getsentry/tools/sentry-cli" >&2
        exit 1
    fi
    if [ -z "${SENTRY_AUTH_TOKEN:-}" ] && [ ! -f "$HOME/.sentryclirc" ]; then
        echo
        echo "error: no Sentry credentials." >&2
        echo "       export SENTRY_AUTH_TOKEN=... (Settings -> Auth Tokens)," >&2
        echo "       or run 'sentry-cli login', or pass --no-upload." >&2
        exit 1
    fi

    # Derive the project from the DSN rather than making the user configure it twice; the
    # DSN already carries it, and sentry-cli accepts numeric ids as well as slugs.
    #
    # The org is deliberately NOT derived. An organization auth token embeds its own org and
    # overrides anything passed on the command line — sentry-cli warns and ignores it — so
    # sending a DSN-derived org produced a confusing warning on every upload while changing
    # nothing. It is passed through only when the user set SENTRY_ORG themselves, which is
    # what a personal (non-org) token needs.
    if [ -z "${SENTRY_PROJECT:-}" ]; then
        if [ -n "${SENTRY_MICRO_DSN:-}" ]; then
            eval "$(python3 - "$SENTRY_MICRO_DSN" <<'PYEOF'
import sys
from urllib.parse import urlparse
project = (urlparse(sys.argv[1]).path or "").strip("/").split("/")[-1]
if project:
    print(f'export SENTRY_PROJECT="{project}"')
PYEOF
)"
        fi
    fi
    if [ -z "${SENTRY_PROJECT:-}" ]; then
        echo
        echo "error: could not determine the project." >&2
        echo "       set SENTRY_MICRO_DSN, or export SENTRY_PROJECT." >&2
        exit 1
    fi
    # Empty unless the user chose one, in which case it is passed through unquoted-expanded.
    ORG_ARGS=""
    [ -n "${SENTRY_ORG:-}" ] && ORG_ARGS="--org ${SENTRY_ORG}"
    echo "  project   ${SENTRY_PROJECT}${SENTRY_ORG:+ (org ${SENTRY_ORG})}"

    echo
    echo "→ uploading debug files to Sentry"
    # --include-sources embeds the source text, so Sentry shows the offending line and not
    # just its number. Harmless for a private project; drop it if the code is not yours.
    # shellcheck disable=SC2086 # ORG_ARGS is intentionally word-split: empty means "omit".
    sentry-cli debug-files upload --include-sources \
        $ORG_ARGS --project "$SENTRY_PROJECT" "$ELF"

    echo
    echo "→ registering the release"
    # Lets Sentry associate events with this build even before any crash arrives. Not fatal
    # if it fails — the debug files are what symbolication actually needs.
    # shellcheck disable=SC2086
    sentry-cli releases new $ORG_ARGS --project "$SENTRY_PROJECT" "$RELEASE" \
        >/dev/null 2>&1 || echo "  (could not register the release; debug files are uploaded)"
fi

if [ "$UPLOAD_FIRMWARE" -eq 1 ]; then
    echo
    echo "→ flashing"
    ( cd "$PROJECT_DIR" && pio run -e "$ENVIRONMENT" -t upload )
fi

echo
echo "done. Events from this build carry:"
echo "  release  ${RELEASE}"
echo "  code_id  ${BUILD_ID}"
echo "  debug_id $(python3 "${REPO_ROOT}/scripts/stamp_build_id.py" "$ELF" \
    --build-id "$BUILD_ID" --objcopy "$OBJCOPY" --print-debug-id)"
