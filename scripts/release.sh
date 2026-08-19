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
WAIT_FOR_PROCESSING=1
INCLUDE_SOURCES=1
SET_COMMITS=1
JSON_SUMMARY=""

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
      --no-wait          return as soon as the upload is accepted, without waiting
                         for Sentry to finish processing it
      --no-sources       upload debug info only, without embedding the source text
      --no-commits       do not tell Sentry which commits went into this build
      --json-summary F   write {env, release, code_id, debug_id, ...} to F
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
        --no-wait) WAIT_FOR_PROCESSING=0; shift ;;
        --no-sources) INCLUDE_SOURCES=0; shift ;;
        --no-commits) SET_COMMITS=0; shift ;;
        --json-summary) JSON_SUMMARY="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v pio >/dev/null 2>&1 || { echo "error: platformio (pio) not found" >&2; exit 1; }

# A relative --project-dir is relative to this repository, which is what a developer
# running this by hand means. The GitHub Action runs it against a project in *another*
# repository's workspace, so an absolute path has to work too.
case "$PROJECT_DIR" in
    /*) PROJECT_PATH="$PROJECT_DIR" ;;
    *)  PROJECT_PATH="${REPO_ROOT}/${PROJECT_DIR}" ;;
esac
if [ ! -f "${PROJECT_PATH}/platformio.ini" ]; then
    echo "error: no platformio.ini in ${PROJECT_PATH}" >&2
    exit 1
fi

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
# Shown as the module beside every frame. Named after the target so a fleet running several
# variants stays readable in the issue stream.
export SENTRY_MICRO_IMAGE_NAME="${ENVIRONMENT}.elf"

ELF="${PROJECT_PATH}/.pio/build/${ENVIRONMENT}/firmware.elf"

# Build until the measurements stop moving — usually three passes, occasionally two.
#
# Sentry maps an address back to a function using `instruction_addr - image_addr`, so the
# firmware has to report the ELF's load address and extent — which only exist once the ELF
# has been linked. Skipping that costs nothing visible and silently produces `<unknown>`
# for every frame, which is a much worse failure than a slow build.
#
# It cannot be done in a fixed two passes, because baking the numbers in *changes* them.
# On Xtensa a small constant is a two-byte `movi.n` while a large one becomes a four-byte
# entry in the literal pool, so compiling in a real 0x00d752cf where pass 1 had nothing
# grew this image by 0x1c bytes — and the size in the firmware then described the binary
# from the previous pass. So: re-measure, re-bake, and repeat until a pass produces an ELF
# whose measurements match the ones it was built with. That is a genuine fixpoint rather
# than an assumption that one relink settles it.
#
# It converges quickly (the size feeds back only through instruction encoding, which stops
# changing once the constants are the same width), so failing to settle means something
# else is non-deterministic and is worth stopping for.
MAX_PASSES=6
PASS=1

echo "→ building (pass ${PASS}: measure the image)"
( cd "$PROJECT_PATH" && pio run -e "$ENVIRONMENT" >/dev/null )
[ -f "$ELF" ] || { echo "error: no ELF at $ELF" >&2; exit 1; }

eval "$(python3 "${REPO_ROOT}/scripts/elf_info.py" "$ELF" --export)"
export SENTRY_MICRO_IMAGE_ADDR SENTRY_MICRO_IMAGE_SIZE
echo "  image_addr ${SENTRY_MICRO_IMAGE_ADDR}"
echo "  image_size ${SENTRY_MICRO_IMAGE_SIZE}"

SETTLED=0
while [ "$PASS" -lt "$MAX_PASSES" ]; do
    PASS=$((PASS + 1))
    echo
    echo "→ building (pass ${PASS}: bake it in)"
    # Quiet except on the last useful pass, so the log is not three identical builds.
    ( cd "$PROJECT_PATH" && pio run -e "$ENVIRONMENT" >/dev/null )

    eval "$(python3 "${REPO_ROOT}/scripts/elf_info.py" "$ELF" --export \
        | sed 's/SENTRY_MICRO_IMAGE/MEASURED_IMAGE/')"
    if [ "$MEASURED_IMAGE_ADDR" = "$SENTRY_MICRO_IMAGE_ADDR" ] && \
       [ "$MEASURED_IMAGE_SIZE" = "$SENTRY_MICRO_IMAGE_SIZE" ]; then
        echo "  settled: ${SENTRY_MICRO_IMAGE_ADDR} +${SENTRY_MICRO_IMAGE_SIZE}"
        SETTLED=1
        break
    fi

    echo "  moved:   ${SENTRY_MICRO_IMAGE_ADDR} +${SENTRY_MICRO_IMAGE_SIZE}" \
         "-> ${MEASURED_IMAGE_ADDR} +${MEASURED_IMAGE_SIZE}"
    SENTRY_MICRO_IMAGE_ADDR="$MEASURED_IMAGE_ADDR"
    SENTRY_MICRO_IMAGE_SIZE="$MEASURED_IMAGE_SIZE"
    export SENTRY_MICRO_IMAGE_ADDR SENTRY_MICRO_IMAGE_SIZE
done

if [ "$SETTLED" -ne 1 ]; then
    echo
    echo "error: the image never stopped moving after ${MAX_PASSES} passes." >&2
    echo "       last: compiled in ${SENTRY_MICRO_IMAGE_ADDR} +${SENTRY_MICRO_IMAGE_SIZE}," >&2
    echo "             measured ${MEASURED_IMAGE_ADDR} +${MEASURED_IMAGE_SIZE}" >&2
    echo "       Shipping this would upload debug files that do not describe the binary." >&2
    echo "       The build is not reproducible; investigate before releasing." >&2
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
DEBUG_ID="$(python3 "${REPO_ROOT}/scripts/stamp_build_id.py" "$ELF" \
    --build-id "$BUILD_ID" --objcopy "$OBJCOPY" --print-debug-id)"

echo
echo "→ verifying the debug file"
# objcopy exits 0 whether or not the note ended up where Sentry looks for it, so read the
# result back with the same library Sentry uses rather than assuming. Only optional when
# we are not uploading — a release that skips this check is how you find out weeks later
# that every frame is <unknown>.
CHECK_ARGS=""
[ "$DO_UPLOAD" -eq 1 ] || CHECK_ARGS="--optional"
# shellcheck disable=SC2086 # CHECK_ARGS is intentionally word-split: empty means "omit".
python3 "${REPO_ROOT}/scripts/check_debug_file.py" "$ELF" --expect-debug-id "$DEBUG_ID" $CHECK_ARGS

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
    # just its number. Harmless for a private project; --no-sources turns it off, which is
    # what you want if the repository is public but the firmware source is not.
    #
    # --id + --require-all turns "nothing matched, uploaded 0 files" — which sentry-cli
    # otherwise reports as success — into an error. --wait makes the *server's* verdict
    # visible: without it the command returns as soon as the bytes are accepted, and a
    # file the server later rejects looks exactly like one it processed happily.
    UPLOAD_ARGS=""
    [ "$WAIT_FOR_PROCESSING" -eq 1 ] && UPLOAD_ARGS="--wait"
    [ "$INCLUDE_SOURCES" -eq 1 ] && UPLOAD_ARGS="$UPLOAD_ARGS --include-sources"
    # shellcheck disable=SC2086 # ORG_ARGS/UPLOAD_ARGS are word-split on purpose: empty means "omit".
    sentry-cli debug-files upload $UPLOAD_ARGS \
        --id "$DEBUG_ID" --require-all \
        $ORG_ARGS --project "$SENTRY_PROJECT" "$ELF"

    echo
    echo "→ registering the release"
    # Lets Sentry associate events with this build even before any crash arrives. None of
    # this is fatal — the debug files are what symbolication actually needs, and a release
    # that failed to register still produces readable backtraces.
    # shellcheck disable=SC2086
    if ! sentry-cli releases new $ORG_ARGS --project "$SENTRY_PROJECT" "$RELEASE" >/dev/null 2>&1
    then
        echo "  (could not register the release; debug files are uploaded)"
    else
        if [ "$SET_COMMITS" -eq 1 ]; then
            # Which commits went into this build. Without it an issue tells you *that* the
            # firmware broke but not *what changed* — and on a device, "what changed" is
            # usually the whole question, because you cannot reproduce it at a breakpoint.
            #
            # Run from the firmware's own repository rather than this one. When the GitHub
            # Action drives this script, $REPO_ROOT is the checked-out sentry-micro and the
            # commits that matter are the adopter's, not ours.
            #
            # --auto prefers a repository configured in Sentry's integrations, which is
            # what unlocks suspect commits and links back to GitHub, and falls back to the
            # local git tree when there is none — where you still get the commit list, just
            # under a repo named after the git remote. --ignore-missing keeps this working
            # when the previous release's commit is absent from the clone, which a shallow
            # CI checkout guarantees.
            # shellcheck disable=SC2086
            ( cd "$PROJECT_PATH" && sentry-cli releases set-commits --auto --ignore-missing \
                $ORG_ARGS --project "$SENTRY_PROJECT" "$RELEASE" ) \
                || echo "  (no commits associated; is $PROJECT_PATH inside a git repository?)"
        fi
        # Without this the release has no ship date, so "regressed in" and the release
        # comparison views have nothing to sort by.
        # shellcheck disable=SC2086
        sentry-cli releases finalize $ORG_ARGS --project "$SENTRY_PROJECT" "$RELEASE" \
            >/dev/null 2>&1 || echo "  (could not finalize the release)"
    fi
fi

if [ "$UPLOAD_FIRMWARE" -eq 1 ]; then
    echo
    echo "→ flashing"
    ( cd "$PROJECT_PATH" && pio run -e "$ENVIRONMENT" -t upload )
fi

# Machine-readable, for the GitHub Action: it collects one of these per matrix variant and
# renders the table showing which variants are covered and under which ids.
if [ -n "$JSON_SUMMARY" ]; then
    python3 - "$JSON_SUMMARY" "$ENVIRONMENT" "$RELEASE" "$BUILD_ID" "$DEBUG_ID" \
        "$SENTRY_MICRO_IMAGE_ADDR" "$SENTRY_MICRO_IMAGE_SIZE" "$ELF" "$DO_UPLOAD" <<'PYEOF'
import json, sys
keys = ("environment", "release", "code_id", "debug_id",
        "image_addr", "image_size", "elf", "uploaded")
summary = dict(zip(keys, sys.argv[2:]))
summary["uploaded"] = summary["uploaded"] == "1"
with open(sys.argv[1], "w") as handle:
    json.dump(summary, handle, indent=2)
PYEOF
fi

echo
echo "done. Events from this build carry:"
echo "  release  ${RELEASE}"
echo "  code_id  ${BUILD_ID}"
echo "  debug_id ${DEBUG_ID}"
