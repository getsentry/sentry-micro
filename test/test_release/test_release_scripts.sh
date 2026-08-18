#!/usr/bin/env bash
#
# Tests for the parts of the release chain that exist to fail loudly.
#
# Everything these scripts guard against fails *silently* in production: a build variant
# nobody uploaded, two variants sharing a debug id, an ELF the stamp never reached. So the
# thing worth testing is not that they succeed — it is that they refuse, with a non-zero
# exit code, in each of those cases. A guard that has quietly stopped guarding looks
# exactly like one that has nothing to complain about.
#
#   test/test_release/test_release_scripts.sh

set -uo pipefail

cd "$(dirname "$0")/../.."
SCRIPTS="$PWD/scripts"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PASSED=0
FAILED=0

# Runs a command, compares its exit code, and prints a one-line verdict.
expect() {
    local want="$1" name="$2"
    shift 2
    local output
    output="$("$@" 2>&1)"
    local got=$?
    if [ "$got" -eq "$want" ]; then
        PASSED=$((PASSED + 1))
        echo "  ok    ${name}"
    else
        FAILED=$((FAILED + 1))
        echo "  FAIL  ${name} (exit ${got}, wanted ${want})"
        echo "${output}" | sed 's/^/          /'
    fi
}

cat > "${WORK}/platformio.ini" <<'INI'
[platformio]
default_envs = alpha

[env]
framework = arduino

[env:alpha]
board = esp32dev

[env:beta]
board = esp32-s3-devkitc-1

[env:gamma]
board = esp32-c3-devkitm-1
INI

echo "list_environments.py"
expect 0 "lists every declared environment" \
    python3 "${SCRIPTS}/list_environments.py" "$WORK"
expect 1 "refuses a partial list — the silent-gap case" \
    python3 "${SCRIPTS}/list_environments.py" "$WORK" --select alpha
expect 0 "accepts a partial list when the rest is skipped on purpose" \
    python3 "${SCRIPTS}/list_environments.py" "$WORK" --select alpha --skip 'beta,gamma'
expect 1 "rejects an environment that is not declared" \
    python3 "${SCRIPTS}/list_environments.py" "$WORK" --select alpha,delta
expect 1 "rejects a typo in --skip rather than silently skipping nothing" \
    python3 "${SCRIPTS}/list_environments.py" "$WORK" --skip bata
expect 1 "rejects a project with no environments at all" \
    python3 "${SCRIPTS}/list_environments.py" "${WORK}/nonexistent"

# The order matters: it becomes the job matrix, and a matrix that reorders between runs
# makes CI history unreadable.
got="$(python3 "${SCRIPTS}/list_environments.py" "$WORK" --json)"
if [ "$got" = '["alpha", "beta", "gamma"]' ]; then
    PASSED=$((PASSED + 1)); echo "  ok    preserves declaration order"
else
    FAILED=$((FAILED + 1)); echo "  FAIL  preserves declaration order: ${got}"
fi

echo
echo "summarize_release.py"
summary() {
    cat > "${WORK}/summaries/${1}.json" <<JSON
{"environment": "${1}", "release": "r@1", "code_id": "${2}0", "debug_id": "${2}",
 "image_addr": "0x3f400020", "image_size": "0xd75333", "elf": "/tmp/${1}.elf",
 "uploaded": true}
JSON
}

mkdir -p "${WORK}/summaries"
summary alpha "11111111-1111-1111-1111-111111111111"
summary beta  "22222222-2222-2222-2222-222222222222"
expect 0 "accepts distinct debug ids" \
    python3 "${SCRIPTS}/summarize_release.py" "${WORK}/summaries"

summary beta "11111111-1111-1111-1111-111111111111"
expect 1 "refuses two variants sharing a debug id" \
    python3 "${SCRIPTS}/summarize_release.py" "${WORK}/summaries"

rm -f "${WORK}/summaries"/*.json
expect 1 "refuses to report a release that built nothing" \
    python3 "${SCRIPTS}/summarize_release.py" "${WORK}/summaries"

echo
echo "check_debug_file.py"
if command -v sentry-cli >/dev/null 2>&1; then
    # An ELF that was never stamped is the common case: objcopy exited 0, the upload would
    # succeed, and every frame would render <unknown>.
    ELF="$(find examples -name firmware.elf 2>/dev/null | head -1)"
    # GNU binutils are per-target: the RISC-V objcopy refuses an Xtensa ELF outright. So
    # probe each candidate with a plain copy and keep the one that recognises the format,
    # rather than assuming any objcopy handles any ELF.
    OBJCOPY=""
    if [ -n "$ELF" ]; then
        for candidate in objcopy \
            $(find "$HOME/.platformio/packages" -type f -name '*-elf-objcopy' 2>/dev/null); do
            command -v "$candidate" >/dev/null 2>&1 || [ -x "$candidate" ] || continue
            if "$candidate" "$ELF" "${WORK}/probe.elf" >/dev/null 2>&1; then
                OBJCOPY="$candidate"
                break
            fi
        done
    fi

    if [ -n "$ELF" ] && [ -n "$OBJCOPY" ]; then
        ID="deadbeef000000000000000000000000cafef00d"
        # Pure function of the build-id, so this cannot silently come back empty the way
        # deriving it from a failed stamp can — which would make the assertions vacuous.
        WANT="$(python3 -c "
import sys; sys.path.insert(0, '${SCRIPTS}')
from stamp_build_id import debug_id_for
print(debug_id_for('${ID}'))")"

        cp "$ELF" "${WORK}/firmware.elf"
        python3 "${SCRIPTS}/stamp_build_id.py" "${WORK}/firmware.elf" \
            --build-id "$ID" --objcopy "$OBJCOPY" >/dev/null

        # A positive case, so that a check which has started passing unconditionally shows
        # up as a failure in the negative cases rather than as a green run.
        expect 0 "accepts an ELF stamped with the id it was told to expect" \
            python3 "${SCRIPTS}/check_debug_file.py" "${WORK}/firmware.elf" \
                --expect-debug-id "$WANT"
        expect 1 "rejects an ELF whose debug id is not the expected one" \
            python3 "${SCRIPTS}/check_debug_file.py" "${WORK}/firmware.elf" \
                --expect-debug-id "00000000-0000-0000-0000-0000deadbeef"

        "$OBJCOPY" --remove-section .note.gnu.build-id "${WORK}/firmware.elf"
        expect 1 "rejects an ELF with no build-id note" \
            python3 "${SCRIPTS}/check_debug_file.py" "${WORK}/firmware.elf"
    else
        echo "  skip  need a built firmware.elf and a matching objcopy"
    fi

    expect 1 "rejects a missing file" \
        python3 "${SCRIPTS}/check_debug_file.py" "${WORK}/nope.elf"
else
    echo "  skip  sentry-cli not installed"
fi
expect 0 "treats a missing sentry-cli as optional when asked to" \
    python3 "${SCRIPTS}/check_debug_file.py" "${WORK}/platformio.ini" \
        --sentry-cli definitely-not-installed --optional

echo
echo "${PASSED} passed, ${FAILED} failed"
[ "$FAILED" -eq 0 ]
