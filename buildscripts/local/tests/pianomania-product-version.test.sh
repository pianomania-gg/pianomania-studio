#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
TEST_SCRIPT="${SCRIPT_DIR}/pianomania-product-version.cmake"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

cmake \
  -DPIANOMANIA_TEST_SOURCE_ROOT="$SOURCE_ROOT" \
  -DPIANOMANIA_MUSESCORE_PRODUCT_VERSION=1.2.3 \
  -DPIANOMANIA_EXPECTED_PRODUCT_VERSION=1.2.3 \
  '-DPIANOMANIA_EXPECTED_APP_TITLE=Pianomania MuseScore 1.2.3' \
  -DMUSE_APP_BUILD_MODE=release \
  -P "$TEST_SCRIPT"

cmake \
  -DPIANOMANIA_TEST_SOURCE_ROOT="$SOURCE_ROOT" \
  -DPIANOMANIA_EXPECTED_PRODUCT_VERSION=0.0.0 \
  '-DPIANOMANIA_EXPECTED_APP_TITLE=Pianomania MuseScore 0.0.0' \
  -DMUSE_APP_BUILD_MODE=dev \
  -P "$TEST_SCRIPT"

if cmake \
  -DPIANOMANIA_TEST_SOURCE_ROOT="$SOURCE_ROOT" \
  -DPIANOMANIA_MUSESCORE_PRODUCT_VERSION=invalid \
  -DPIANOMANIA_EXPECTED_PRODUCT_VERSION=invalid \
  '-DPIANOMANIA_EXPECTED_APP_TITLE=Pianomania MuseScore invalid' \
  -DMUSE_APP_BUILD_MODE=release \
  -P "$TEST_SCRIPT" >"$TEST_ROOT/invalid-version.log" 2>&1; then
  printf 'Expected an invalid Pianomania MuseScore product version to fail.\n' >&2
  exit 1
fi
grep -F 'PIANOMANIA_MUSESCORE_PRODUCT_VERSION must be a semantic version' \
  "$TEST_ROOT/invalid-version.log" >/dev/null

if cmake \
  -DPIANOMANIA_TEST_SOURCE_ROOT="$SOURCE_ROOT" \
  -DPIANOMANIA_EXPECTED_PRODUCT_VERSION=0.0.0 \
  '-DPIANOMANIA_EXPECTED_APP_TITLE=Pianomania MuseScore 0.0.0' \
  -DMUSE_APP_BUILD_MODE=release \
  -P "$TEST_SCRIPT" >"$TEST_ROOT/missing-release-version.log" 2>&1; then
  printf 'Expected a release without an exact product version to fail.\n' >&2
  exit 1
fi
grep -F 'Release builds require an exact PIANOMANIA_MUSESCORE_PRODUCT_VERSION' \
  "$TEST_ROOT/missing-release-version.log" >/dev/null

printf 'Pianomania MuseScore product-version configuration checks passed.\n'
