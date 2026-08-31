#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  package-macos-dev.sh --build-dir <path> --qt-dir <path> --output-app <path> --output-zip <path> [--architecture arm64] [--sign-identity <identity>] [--notarize ...]

Options:
  --build-dir      Build directory containing install/mscore.app.
  --qt-dir         Qt installation root, for example ~/Qt/6.10.2/macos.
  --output-app     Final .app destination.
  --output-zip     Final .zip destination.
  --architecture   Thin every bundled Mach-O file to this architecture. Only arm64 is supported.
  --sign-identity  Codesign identity. Defaults to - for ad hoc signing, or auto-detected with --notarize.
  --notarize       Submit the signed app to Apple notarization, staple it, and validate Gatekeeper.
  --notary-profile Keychain profile created by xcrun notarytool store-credentials.
  --apple-id       Apple ID for notarization. Used with --team-id and --password.
  --team-id        Apple Developer Team ID for notarization.
  --password       App-specific password for notarization.
EOF
}

BUILD_DIR=""
QT_DIR=""
OUTPUT_APP=""
OUTPUT_ZIP=""
TARGET_ARCHITECTURE=""
SIGN_IDENTITY="-"
NOTARIZE=0
NOTARY_PROFILE=""
APPLE_ID=""
APPLE_TEAM_ID=""
APPLE_PASSWORD=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="${2:-}"
            shift 2
            ;;
        --qt-dir)
            QT_DIR="${2:-}"
            shift 2
            ;;
        --output-app)
            OUTPUT_APP="${2:-}"
            shift 2
            ;;
        --output-zip)
            OUTPUT_ZIP="${2:-}"
            shift 2
            ;;
        --architecture)
            TARGET_ARCHITECTURE="${2:-}"
            shift 2
            ;;
        --sign-identity)
            SIGN_IDENTITY="${2:-}"
            shift 2
            ;;
        --notarize)
            NOTARIZE=1
            shift
            ;;
        --notary-profile)
            NOTARY_PROFILE="${2:-}"
            shift 2
            ;;
        --apple-id)
            APPLE_ID="${2:-}"
            shift 2
            ;;
        --team-id)
            APPLE_TEAM_ID="${2:-}"
            shift 2
            ;;
        --password)
            APPLE_PASSWORD="${2:-}"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$BUILD_DIR" || -z "$QT_DIR" || -z "$OUTPUT_APP" || -z "$OUTPUT_ZIP" ]]; then
    usage >&2
    exit 1
fi

if [[ -n "$TARGET_ARCHITECTURE" && "$TARGET_ARCHITECTURE" != "arm64" ]]; then
    echo "Unsupported package architecture: $TARGET_ARCHITECTURE" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOURCE_APP="$BUILD_DIR/install/mscore.app"
MACDEPLOYQT_BIN="$QT_DIR/bin/macdeployqt"

if [[ ! -d "$SOURCE_APP" ]]; then
    echo "App bundle not found at: $SOURCE_APP" >&2
    exit 1
fi

if [[ ! -x "$MACDEPLOYQT_BIN" ]]; then
    echo "macdeployqt not found at: $MACDEPLOYQT_BIN" >&2
    exit 1
fi

for required_tool in codesign ditto file find lipo otool; do
    if ! command -v "$required_tool" >/dev/null 2>&1; then
        echo "Required tool is missing: $required_tool" >&2
        exit 1
    fi
done

if [[ "$NOTARIZE" -eq 1 ]]; then
    if [[ "$SIGN_IDENTITY" == "-" ]]; then
        if ! command -v security >/dev/null 2>&1; then
            echo "Required tool is missing: security" >&2
            exit 1
        fi

        SIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null | sed -n 's/.*"\(Developer ID Application: .*\)".*/\1/p' | head -n 1)"
        if [[ -z "$SIGN_IDENTITY" ]]; then
            echo "Notarization requires a Developer ID Application certificate in the login keychain or --sign-identity." >&2
            exit 1
        fi
        echo "Using Developer ID signing identity: $SIGN_IDENTITY"
    fi

    if ! command -v xcrun >/dev/null 2>&1; then
        echo "Required tool is missing: xcrun" >&2
        exit 1
    fi

    if ! command -v spctl >/dev/null 2>&1; then
        echo "Required tool is missing: spctl" >&2
        exit 1
    fi

    if [[ -z "$NOTARY_PROFILE" && ( -z "$APPLE_ID" || -z "$APPLE_TEAM_ID" || -z "$APPLE_PASSWORD" ) ]]; then
        echo "Notarization requires --notary-profile or all of --apple-id, --team-id, and --password." >&2
        exit 1
    fi
fi

copy_dir_if_missing() {
    local source_path="$1"
    local destination_path="$2"

    if [[ ! -e "$destination_path" ]]; then
        echo "Copying missing dependency: $destination_path"
        mkdir -p "$(dirname "$destination_path")"
        ditto "$source_path" "$destination_path"
    fi
}

ensure_qt_conf_paths() {
    local qt_conf_path="$1"

    cat >"$qt_conf_path" <<'EOF'
[Paths]
Plugins = PlugIns
Imports = Resources/qml_mu
Qml2Imports = Resources/qml_mu
EOF
}

codesign_path() {
    local target_path="$1"
    echo "Signing: $target_path"
    if [[ "$SIGN_IDENTITY" == "-" ]]; then
        codesign --force --sign "$SIGN_IDENTITY" "$target_path"
    else
        codesign --force --options runtime --sign "$SIGN_IDENTITY" "$target_path"
    fi
}

codesign_app_bundle() {
    local target_path="$1"
    echo "Signing app bundle: $target_path"
    if [[ "$SIGN_IDENTITY" == "-" ]]; then
        codesign --force --sign "$SIGN_IDENTITY" "$target_path"
    else
        codesign --force --options runtime --entitlements "$ENTITLEMENTS_PATH" --deep --sign "$SIGN_IDENTITY" "$target_path"
    fi
}

codesign_nested_bundle() {
    local target_path="$1"
    echo "Signing nested bundle: $target_path"
    if [[ "$SIGN_IDENTITY" == "-" ]]; then
        codesign --force --sign "$SIGN_IDENTITY" "$target_path"
    else
        codesign --force --options runtime --sign "$SIGN_IDENTITY" "$target_path"
    fi
}

notarize_app_bundle() {
    local target_path="$1"
    local notarization_zip="$STAGE_ROOT/notarization.zip"
    local notary_args=()

    echo "Creating notarization ZIP..."
    ditto -c -k --sequesterRsrc --keepParent "$target_path" "$notarization_zip"

    if [[ -n "$NOTARY_PROFILE" ]]; then
        notary_args=(--keychain-profile "$NOTARY_PROFILE")
    else
        notary_args=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_PASSWORD")
    fi

    echo "Submitting app for notarization..."
    xcrun notarytool submit "${notary_args[@]}" --wait "$notarization_zip"

    echo "Stapling notarization ticket..."
    xcrun stapler staple "$target_path"
    xcrun stapler validate "$target_path"

    echo "Assessing stapled app with Gatekeeper..."
    spctl --assess --type execute --verbose=4 "$target_path"
}

STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/mscore-package.XXXXXX")"
cleanup() {
    rm -rf "$STAGE_ROOT"
}
trap cleanup EXIT

STAGED_APP="$STAGE_ROOT/mscore.app"
QML_DIR="$STAGED_APP/Contents/Resources/qml"
QML_MU_DIR="$STAGED_APP/Contents/Resources/qml_mu"
FRAMEWORKS_DIR="$STAGED_APP/Contents/Frameworks"
QT_CONF_PATH="$STAGED_APP/Contents/Resources/qt.conf"
ENTITLEMENTS_PATH="$REPO_ROOT/buildscripts/packaging/macOS/entitlements.plist"

echo "Staging app bundle..."
ditto "$SOURCE_APP" "$STAGED_APP"

# Pianomania: drop local symbol tables from the executable before signing.
# This is a ~30MB saving with no functional change; crash symbolication uses
# the build tree, not the shipped binary.
echo "Stripping executable symbols..."
strip -x "$STAGED_APP/Contents/MacOS/mscore"

echo "Running macdeployqt..."
"$MACDEPLOYQT_BIN" "$STAGED_APP" -qmldir="$REPO_ROOT" -verbose=1

echo "Copying targeted fallback Qt dependencies..."
copy_dir_if_missing "$QT_DIR/qml/QtQuick" "$QML_DIR/QtQuick"
copy_dir_if_missing "$QT_DIR/qml/QtQml" "$QML_DIR/QtQml"
copy_dir_if_missing "$QT_DIR/qml/Qt5Compat" "$QML_DIR/Qt5Compat"
copy_dir_if_missing "$QT_DIR/qml/Qt/labs/platform" "$QML_DIR/Qt/labs/platform"
copy_dir_if_missing "$QT_DIR/lib/QtQuickControls2Impl.framework" "$FRAMEWORKS_DIR/QtQuickControls2Impl.framework"
copy_dir_if_missing "$QT_DIR/lib/QtQuickTemplates2.framework" "$FRAMEWORKS_DIR/QtQuickTemplates2.framework"
copy_dir_if_missing "$QT_DIR/lib/QtQmlWorkerScript.framework" "$FRAMEWORKS_DIR/QtQmlWorkerScript.framework"
copy_dir_if_missing "$QT_DIR/lib/QtQuickWidgets.framework" "$FRAMEWORKS_DIR/QtQuickWidgets.framework"
copy_dir_if_missing "$QT_DIR/lib/QtCore5Compat.framework" "$FRAMEWORKS_DIR/QtCore5Compat.framework"

echo "Pruning nonessential Qt SQL drivers..."
rm -f \
    "$STAGED_APP/Contents/PlugIns/sqldrivers/libqsqlmimer.dylib" \
    "$STAGED_APP/Contents/PlugIns/sqldrivers/libqsqlodbc.dylib" \
    "$STAGED_APP/Contents/PlugIns/sqldrivers/libqsqlpsql.dylib"

echo "Bundling external Homebrew dylib dependencies..."
bash "$REPO_ROOT/buildscripts/packaging/macOS/bundle-homebrew-dylibs.sh" "$STAGED_APP"

if [[ -d "$QML_DIR" ]]; then
    echo "Renaming qml directory to qml_mu..."
    rm -rf "$QML_MU_DIR"
    mv "$QML_DIR" "$QML_MU_DIR"
fi

if [[ "$TARGET_ARCHITECTURE" == "arm64" ]]; then
    echo "Preparing arm64-only application code..."
    bash "$SCRIPT_DIR/prepare-macos-arm64-app.sh" "$STAGED_APP"
fi

echo "Writing qt.conf..."
ensure_qt_conf_paths "$QT_CONF_PATH"

echo "Signing nested frameworks..."
while IFS= read -r framework_path; do
    codesign_path "$framework_path"
done < <(find "$FRAMEWORKS_DIR" -maxdepth 1 -type d -name '*.framework' | sort)

echo "Signing bundled framework dynamic libraries..."
while IFS= read -r dylib_path; do
    codesign_path "$dylib_path"
done < <(find "$FRAMEWORKS_DIR" -maxdepth 1 -type f \( -name '*.dylib' -o -name '*.so' \) | sort)

echo "Signing nested dynamic libraries..."
while IFS= read -r dylib_path; do
    codesign_path "$dylib_path"
done < <(find "$STAGED_APP/Contents" \( -path "$FRAMEWORKS_DIR" -prune \) -o -type f \( -name '*.dylib' -o -name '*.so' \) -print | sort)

echo "Signing nested code bundles..."
while IFS= read -r bundle_path; do
    codesign_nested_bundle "$bundle_path"
done < <(find "$STAGED_APP/Contents" -type d \( -name '*.appex' -o -name '*.xpc' -o -name '*.app' \) -print \
    | awk '{ print length($0), $0 }' | sort -rn | cut -d' ' -f2-)

echo "Signing app bundle..."
codesign_app_bundle "$STAGED_APP"

echo "Verifying staged app bundle..."
codesign --verify --deep --strict --verbose=4 "$STAGED_APP"

if [[ "$NOTARIZE" -eq 1 ]]; then
    notarize_app_bundle "$STAGED_APP"
fi

echo "Stamping app bundle timestamp..."
touch "$STAGED_APP"

mkdir -p "$(dirname "$OUTPUT_APP")" "$(dirname "$OUTPUT_ZIP")"
rm -rf "$OUTPUT_APP"
rm -f "$OUTPUT_ZIP"

echo "Publishing app bundle..."
ditto "$STAGED_APP" "$OUTPUT_APP"

echo "Creating share-safe ZIP..."
ditto -c -k --sequesterRsrc --keepParent "$STAGED_APP" "$OUTPUT_ZIP"

echo "Package complete."
echo "App: $OUTPUT_APP"
echo "Zip: $OUTPUT_ZIP"
