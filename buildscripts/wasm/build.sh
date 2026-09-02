#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Build the headless Pianomania WebAssembly converter (pm-converter.js/.wasm) and
# copy the artifacts into the WebApp so the beta-portal Converter tab can serve them.
#
# Prerequisites (install once, pin versions — version skew between emsdk and the
# Qt-for-WebAssembly build is the #1 cause of link failures):
#   * emsdk (Emscripten) 3.1.56      — the version Qt 6.8 is built against
#   * Qt 6.8.3 for WebAssembly (single-threaded)
# NOTE: The converter retains Qt 6.8.3 because its Emscripten toolchain is pinned.
# The desktop and release build use Qt 6.10.2. The headless converter excludes GUI/QML.
#
# Required environment variables:
#   EMSDK              path to the activated emsdk (emcc/emcmake on PATH)
#   QT_WASM_DIR        path to the Qt wasm prefix (…/Qt/6.8.3/wasm_singlethread)
#
# Usage:
#   source <emsdk>/emsdk_env.sh
#   QT_WASM_DIR=~/Qt/6.8.3/wasm_singlethread ./buildscripts/wasm/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MUSESCORE_ROOT="$(cd "$HERE/../.." && pwd)"
REPO_ROOT="$(cd "$MUSESCORE_ROOT/.." && pwd)"
BUILD_DIR="$MUSESCORE_ROOT/build.wasm"
WEBAPP_WASM_DIR="$REPO_ROOT/WebApp/wwwroot/wasm"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "ERROR: emcmake not found. Did you 'source <emsdk>/emsdk_env.sh'?" >&2
    exit 1
fi
: "${QT_WASM_DIR:?Set QT_WASM_DIR to your Qt 6.8.3 wasm_singlethread prefix}"

EMSCRIPTEN_TOOLCHAIN="${EMSDK:-}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
QT_TOOLCHAIN="$QT_WASM_DIR/lib/cmake/Qt6/qt.toolchain.cmake"
QT_HOST_DIR="${QT_HOST_DIR:-$(dirname "$QT_WASM_DIR")/macos}"
QT_QMAKE="$QT_WASM_DIR/bin/qmake6"

for required in "$EMSCRIPTEN_TOOLCHAIN" "$QT_TOOLCHAIN" "$QT_QMAKE"; do
    if [ ! -f "$required" ]; then
        echo "ERROR: required WASM build input is missing: $required" >&2
        exit 1
    fi
done
if [ ! -d "$QT_HOST_DIR" ]; then
    echo "ERROR: matching Qt host kit is missing: $QT_HOST_DIR" >&2
    exit 1
fi

echo "==> Configuring (headless wasm profile)"
cmake -S "$MUSESCORE_ROOT" -B "$BUILD_DIR" \
    -C "$MUSESCORE_ROOT/buildscripts/wasm/wasm-headless.cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$QT_TOOLCHAIN" \
    -DQT_CHAINLOAD_TOOLCHAIN_FILE="$EMSCRIPTEN_TOOLCHAIN" \
    -DQT_HOST_PATH="$QT_HOST_DIR" \
    -DCMAKE_PREFIX_PATH="$QT_WASM_DIR" \
    -DQMAKE="$QT_QMAKE" \
    -DQT_QMAKE_EXECUTABLE="$QT_QMAKE" \
    -DEMCC_CMAKE_TOOLCHAIN="$EMSCRIPTEN_TOOLCHAIN" \
    -DEMCC_EMBED_FONTS_DIR="$MUSESCORE_ROOT/fonts" \
    -DCMAKE_BUILD_TYPE=Release

echo "==> Building"
cmake --build "$BUILD_DIR" --parallel --target \
    muse_global muse_draw muse_network muse_diagnostics \
    muse_actions muse_accessibility muse_midi muse_mpe \
    engraving context commonscene beatroot iex_mei iex_midi pmwasm \
    MuseScoreStudio

echo "==> Collecting artifacts"
mkdir -p "$WEBAPP_WASM_DIR"
# The app target emits pm-converter.js + pm-converter.wasm into public_html.
# (Rename here if the app target name differs; see notes in build.ps1.)
found=0
while IFS= read -r -d '' js; do
    base="${js%.js}"
    cp -f "$js" "$WEBAPP_WASM_DIR/pm-converter.js"
    cp -f "$base.wasm" "$WEBAPP_WASM_DIR/pm-converter.wasm"
    found=1
done < <(find "$BUILD_DIR" -name '*.js' -path '*public_html*' -print0)

if [ "$found" -ne 1 ]; then
    echo "ERROR: could not find the emitted .js/.wasm in $BUILD_DIR. Check the app target output." >&2
    exit 1
fi

# The converter is a GPL binary, so whoever runs it must be able to get the
# source it was built from. Print the commit and the tag that has to be
# published alongside it; see WebApp/wwwroot/wasm/README.md in the monorepo.
WASM_SHA256="$(shasum -a 256 "$WEBAPP_WASM_DIR/pm-converter.wasm" | cut -d' ' -f1)"
SOURCE_COMMIT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse HEAD 2>/dev/null || echo unknown)"
SOURCE_DIRTY=""
if ! git -C "$(dirname "${BASH_SOURCE[0]}")" diff --quiet 2>/dev/null; then
    SOURCE_DIRTY=" (working tree has uncommitted changes)"
fi

echo "==> Done. Artifacts in $WEBAPP_WASM_DIR"
ls -lh "$WEBAPP_WASM_DIR"
echo
echo "==> Corresponding source"
echo "    expanded WASM SHA-256: $WASM_SHA256"
echo "    source commit:         $SOURCE_COMMIT$SOURCE_DIRTY"
echo "    publish this source as: composer-wasm-${WASM_SHA256:0:8}"
echo "    then record the tag in WebApp/wwwroot/wasm/README.md and on the Composer page."
