#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREPARER="${SCRIPT_DIR}/../prepare-macos-arm64-app.sh"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/prepare-macos-arm64-app-test.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT

for required_tool in lipo xcrun; do
    if ! command -v "$required_tool" >/dev/null 2>&1; then
        printf 'Required test tool is missing: %s\n' "$required_tool" >&2
        exit 1
    fi
done

SOURCE_PATH="${TEST_ROOT}/fixture.c"
ARM64_BINARY="${TEST_ROOT}/fixture-arm64"
X86_64_BINARY="${TEST_ROOT}/fixture-x86_64"
UNIVERSAL_BINARY="${TEST_ROOT}/fixture-universal"

printf '%s\n' 'int main(void) { return 0; }' >"$SOURCE_PATH"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=15.0 "$SOURCE_PATH" -o "$ARM64_BINARY"
xcrun --sdk macosx clang -arch x86_64 -mmacosx-version-min=15.0 "$SOURCE_PATH" -o "$X86_64_BINARY"
lipo -create "$ARM64_BINARY" "$X86_64_BINARY" -output "$UNIVERSAL_BINARY"

APP_PATH="${TEST_ROOT}/Prepared.app"
mkdir -p \
    "${APP_PATH}/Contents/MacOS" \
    "${APP_PATH}/Contents/Frameworks" \
    "${APP_PATH}/Contents/Resources/example.dSYM/Contents/Resources/DWARF"
cp "$UNIVERSAL_BINARY" "${APP_PATH}/Contents/MacOS/prepared"
cp "$ARM64_BINARY" "${APP_PATH}/Contents/Frameworks/already-arm64.dylib"
cp "$UNIVERSAL_BINARY" \
    "${APP_PATH}/Contents/Resources/example.dSYM/Contents/Resources/DWARF/example"

bash "$PREPARER" "$APP_PATH"

[[ "$(lipo -archs "${APP_PATH}/Contents/MacOS/prepared" | xargs)" == "arm64" ]]
[[ "$(lipo -archs "${APP_PATH}/Contents/Frameworks/already-arm64.dylib" | xargs)" == "arm64" ]]
[[ ! -e "${APP_PATH}/Contents/Resources/example.dSYM" ]]

BAD_APP_PATH="${TEST_ROOT}/Rejected.app"
mkdir -p "${BAD_APP_PATH}/Contents/MacOS"
cp "$X86_64_BINARY" "${BAD_APP_PATH}/Contents/MacOS/rejected"
if rejection_output="$(bash "$PREPARER" "$BAD_APP_PATH" 2>&1)"; then
    printf 'Expected an x86_64-only application bundle to fail.\n' >&2
    exit 1
fi
grep -F 'Mach-O file lacks required arm64 architecture:' <<<"$rejection_output" >/dev/null

CLASSIFICATION_APP_PATH="${TEST_ROOT}/ClassificationError.app"
FAKE_BIN_PATH="${TEST_ROOT}/fake-bin"
mkdir -p "${CLASSIFICATION_APP_PATH}/Contents/MacOS" "$FAKE_BIN_PATH"
cp "$ARM64_BINARY" "${CLASSIFICATION_APP_PATH}/Contents/MacOS/prepared"
printf 'classification fixture\n' >"${CLASSIFICATION_APP_PATH}/Contents/classification-error"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'last_argument="${!#}"' \
    'error_exit=0' \
    'for argument in "$@"; do [[ "$argument" == "-E" ]] && error_exit=1; done' \
    'if [[ "$last_argument" == *classification-error ]]; then' \
    '    if [[ "$error_exit" -eq 1 ]]; then exit 9; fi' \
    '    printf "cannot open %s\n" "$last_argument"' \
    '    exit 0' \
    'fi' \
    'exec "$REAL_FILE" "$@"' \
    >"${FAKE_BIN_PATH}/file"
chmod +x "${FAKE_BIN_PATH}/file"
if classification_output="$(
    REAL_FILE="$(command -v file)" \
    PATH="${FAKE_BIN_PATH}:${PATH}" \
    bash "$PREPARER" "$CLASSIFICATION_APP_PATH" 2>&1
)"; then
    printf 'Expected an unclassifiable application bundle file to fail.\n' >&2
    exit 1
fi
grep -F 'Could not classify application bundle file:' <<<"$classification_output" >/dev/null

printf 'prepare-macos-arm64-app test passed.\n'
