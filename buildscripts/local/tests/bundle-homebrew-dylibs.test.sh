#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUNDLER="$REPO_ROOT/buildscripts/packaging/macOS/bundle-homebrew-dylibs.sh"

for required_tool in clang install_name_tool otool; do
    if ! command -v "$required_tool" >/dev/null 2>&1; then
        echo "Skipping test because required tool is missing: $required_tool"
        exit 0
    fi
done

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/musescore-bundle-dylibs-test.XXXXXX")"
TMP_DIR="$(cd "$TMP_DIR" && pwd -P)"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

PREFIX="$TMP_DIR/homebrew"
APP_PATH="$TMP_DIR/Fake.app"
mkdir -p \
    "$PREFIX/opt/libalpha/lib" \
    "$PREFIX/opt/libbeta/lib" \
    "$APP_PATH/Contents/MacOS"

printf '%s\n' 'int beta(void) { return 41; }' >"$TMP_DIR/beta.c"
clang -dynamiclib \
    -install_name "$PREFIX/opt/libbeta/lib/libbeta.0.dylib" \
    "$TMP_DIR/beta.c" \
    -o "$PREFIX/opt/libbeta/lib/libbeta.0.dylib"

printf '%s\n' 'int beta(void); int alpha(void) { return beta() + 1; }' >"$TMP_DIR/alpha.c"
clang -dynamiclib \
    -Wl,-headerpad_max_install_names \
    -install_name "$PREFIX/opt/libalpha/lib/libalpha.1.dylib" \
    "$TMP_DIR/alpha.c" \
    "$PREFIX/opt/libbeta/lib/libbeta.0.dylib" \
    -o "$PREFIX/opt/libalpha/lib/libalpha.1.dylib"

printf '%s\n' 'int alpha(void); int main(void) { return alpha() == 42 ? 0 : 1; }' >"$TMP_DIR/main.c"
clang \
    -Wl,-headerpad_max_install_names \
    "$TMP_DIR/main.c" \
    "$PREFIX/opt/libalpha/lib/libalpha.1.dylib" \
    -o "$APP_PATH/Contents/MacOS/fake"

install_name_tool \
    -change "$PREFIX/opt/libalpha/lib/libalpha.1.dylib" "$PREFIX/*/libalpha.1.dylib" \
    "$APP_PATH/Contents/MacOS/fake"

MUSESCORE_BUNDLE_DYLIB_PREFIXES="$PREFIX" "$BUNDLER" "$APP_PATH"

if [[ ! -f "$APP_PATH/Contents/Frameworks/libalpha.1.dylib" || ! -f "$APP_PATH/Contents/Frameworks/libbeta.0.dylib" ]]; then
    echo "Expected bundled dylibs were not copied into Contents/Frameworks." >&2
    exit 1
fi

if otool -L "$APP_PATH/Contents/MacOS/fake" "$APP_PATH/Contents/Frameworks/"*.dylib | grep -F "$PREFIX"; then
    echo "Bundled app still references the external dependency prefix." >&2
    exit 1
fi

"$APP_PATH/Contents/MacOS/fake"

echo "bundle-homebrew-dylibs test passed."
