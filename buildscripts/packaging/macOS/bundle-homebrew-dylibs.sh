#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  bundle-homebrew-dylibs.sh <path-to-app-bundle>

Copies Homebrew dynamic libraries referenced by Mach-O files in a macOS app
bundle into Contents/Frameworks and rewrites their load commands to use
@executable_path/../Frameworks.

Set MUSESCORE_BUNDLE_DYLIB_PREFIXES to a colon-separated prefix list to override
the default external dependency roots. The default is /opt/homebrew:/usr/local.
EOF
}

APP_PATH="${1:-}"

if [[ -z "$APP_PATH" || "$APP_PATH" == "--help" || "$APP_PATH" == "-h" ]]; then
    usage
    exit 0
fi

if [[ ! -d "$APP_PATH/Contents/MacOS" ]]; then
    echo "App bundle is missing Contents/MacOS: $APP_PATH" >&2
    exit 1
fi

for required_tool in chmod cp file find install_name_tool lipo mkdir sort; do
    if ! command -v "$required_tool" >/dev/null 2>&1; then
        echo "Required tool is missing: $required_tool" >&2
        exit 1
    fi
done

# Older macdeployqt builds may need a header-filtering otool wrapper in PATH.
# The bundler needs unfiltered output so Mach-O files without dependencies do
# not turn that wrapper's empty grep result into a fatal error under pipefail.
OTOOL_BIN="${MUSESCORE_OTOOL_BIN:-/usr/bin/otool}"
if [[ ! -x "$OTOOL_BIN" ]]; then
    echo "Required tool is missing or not executable: $OTOOL_BIN" >&2
    exit 1
fi

FRAMEWORKS_DIR="$APP_PATH/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"

IFS=':' read -r -a RAW_BUNDLE_PREFIXES <<<"${MUSESCORE_BUNDLE_DYLIB_PREFIXES:-/opt/homebrew:/usr/local}"
BUNDLE_PREFIXES=()

for raw_prefix in "${RAW_BUNDLE_PREFIXES[@]}"; do
    [[ -z "$raw_prefix" ]] && continue
    if [[ -d "$raw_prefix" ]]; then
        BUNDLE_PREFIXES+=("$(cd "$raw_prefix" && pwd -P)")
    else
        BUNDLE_PREFIXES+=("$raw_prefix")
    fi
done

is_mach_o_file() {
    local file_path="$1"
    [[ -f "$file_path" ]] && file "$file_path" | grep -q 'Mach-O'
}

mach_o_files() {
    find "$APP_PATH/Contents" -type f -print0 |
        while IFS= read -r -d '' file_path; do
            if is_mach_o_file "$file_path"; then
                printf '%s\n' "$file_path"
            fi
        done | sort
}

dependency_id() {
    local file_path="$1"
    "$OTOOL_BIN" -D "$file_path" 2>/dev/null | sed -E '/:$/d' | sed -n '1p'
}

dependencies() {
    local file_path="$1"
    "$OTOOL_BIN" -L "$file_path" | sed -E '/:$/d' | sed -E 's/^[[:space:]]*//' | sed -E 's/[[:space:]]+\(.*$//'
}

is_bundle_candidate() {
    local dependency="$1"
    local prefix

    [[ "$dependency" = @* ]] && return 1
    [[ "$dependency" == /usr/lib/* ]] && return 1
    [[ "$dependency" == /System/Library/* ]] && return 1

    for prefix in "${BUNDLE_PREFIXES[@]}"; do
        [[ -z "$prefix" ]] && continue
        if [[ "$dependency" == "$prefix/"* || "$dependency" == "$prefix"*'*'* ]]; then
            return 0
        fi
    done

    return 1
}

resolve_dependency() {
    local dependency="$1"
    local basename
    local prefix
    local match

    if [[ -f "$dependency" ]]; then
        printf '%s\n' "$dependency"
        return 0
    fi

    if [[ "$dependency" == *'*'* ]]; then
        while IFS= read -r match; do
            if [[ -f "$match" ]]; then
                printf '%s\n' "$match"
                return 0
            fi
        done < <(compgen -G "$dependency" || true)
    fi

    basename="$(basename "$dependency")"

    for prefix in "${BUNDLE_PREFIXES[@]}"; do
        [[ -z "$prefix" || ! -d "$prefix" ]] && continue

        for candidate in "$prefix/lib/$basename" "$prefix"/opt/*/lib/"$basename"; do
            if [[ -f "$candidate" ]]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done

        match="$(find "$prefix" -path "*/$basename" -type f -print -quit 2>/dev/null || true)"
        if [[ -n "$match" ]]; then
            printf '%s\n' "$match"
            return 0
        fi
    done

    return 1
}

copy_dependency() {
    local source_path="$1"
    local bundled_name="$2"
    local destination_path="$FRAMEWORKS_DIR/$bundled_name"

    if [[ ! -f "$destination_path" ]]; then
        echo "Bundling dylib: $source_path -> $destination_path"
        cp -pL "$source_path" "$destination_path"
        chmod u+w "$destination_path"
        install_name_tool -id "@executable_path/../Frameworks/$bundled_name" "$destination_path"
        printf '%s\n' "$destination_path"
        return 0
    fi

    printf '%s\n' "$destination_path"
    return 0
}

validate_architecture_compatibility() {
    local consumer_path="$1"
    local source_path="$2"
    local dependency="$3"
    local consumer_archs
    local source_archs
    local arch

    consumer_archs="$(lipo -archs "$consumer_path" 2>/dev/null || true)"
    source_archs="$(lipo -archs "$source_path" 2>/dev/null || true)"

    if [[ -z "$consumer_archs" || -z "$source_archs" ]]; then
        return 0
    fi

    for arch in $consumer_archs; do
        if [[ " $source_archs " != *" $arch "* ]]; then
            echo "Bundled dylib is missing required architecture '$arch': $source_path" >&2
            echo "Required by: $consumer_path" >&2
            echo "Dependency load path: $dependency" >&2
            echo "Consumer architectures: $consumer_archs" >&2
            echo "Dylib architectures: $source_archs" >&2
            exit 1
        fi
    done
}

rewrite_dependency() {
    local file_path="$1"
    local old_dependency="$2"
    local bundled_name="$3"

    echo "Rewriting dylib load: $file_path"
    echo "  $old_dependency -> @executable_path/../Frameworks/$bundled_name"
    chmod u+w "$file_path"
    install_name_tool -change "$old_dependency" "@executable_path/../Frameworks/$bundled_name" "$file_path"
}

bundle_pass() {
    local changed=1
    local pass
    local file_path
    local file_list
    local install_id
    local dependency
    local dependency_list
    local source_path
    local bundled_name
    local pass_changed

    for pass in {1..20}; do
        pass_changed=0

        file_list="$(mktemp "${TMPDIR:-/tmp}/mscore-mach-o.XXXXXX")"
        mach_o_files >"$file_list"

        while IFS= read -r file_path; do
            install_id="$(dependency_id "$file_path" || true)"

            dependency_list="$(mktemp "${TMPDIR:-/tmp}/mscore-dylib-deps.XXXXXX")"
            dependencies "$file_path" >"$dependency_list"

            while IFS= read -r dependency; do
                [[ -z "$dependency" || "$dependency" == "$install_id" ]] && continue
                is_bundle_candidate "$dependency" || continue

                if ! source_path="$(resolve_dependency "$dependency")"; then
                    echo "Could not resolve external dylib dependency: $dependency" >&2
                    echo "Referenced by: $file_path" >&2
                    exit 1
                fi

                bundled_name="$(basename "$dependency")"
                validate_architecture_compatibility "$file_path" "$source_path" "$dependency"
                copy_dependency "$source_path" "$bundled_name" >/dev/null
                rewrite_dependency "$file_path" "$dependency" "$bundled_name"
                pass_changed=1
            done <"$dependency_list"

            rm -f "$dependency_list"
        done <"$file_list"

        rm -f "$file_list"

        if [[ "$pass_changed" -eq 0 ]]; then
            changed=0
            break
        fi
    done

    if [[ "$changed" -ne 0 ]]; then
        echo "External dylib dependency bundling did not converge." >&2
        exit 1
    fi
}

verify_no_external_dependencies() {
    local file_path
    local file_list
    local install_id
    local dependency
    local dependency_list
    local failed=0

    file_list="$(mktemp "${TMPDIR:-/tmp}/mscore-mach-o.XXXXXX")"
    mach_o_files >"$file_list"

    while IFS= read -r file_path; do
        install_id="$(dependency_id "$file_path" || true)"

        dependency_list="$(mktemp "${TMPDIR:-/tmp}/mscore-dylib-deps.XXXXXX")"
        dependencies "$file_path" >"$dependency_list"

        while IFS= read -r dependency; do
            [[ -z "$dependency" || "$dependency" == "$install_id" ]] && continue
            if is_bundle_candidate "$dependency"; then
                echo "Unbundled external dylib remains in $file_path: $dependency" >&2
                failed=1
            fi
        done <"$dependency_list"

        rm -f "$dependency_list"
    done <"$file_list"

    rm -f "$file_list"

    if [[ "$failed" -ne 0 ]]; then
        exit 1
    fi
}

bundle_pass
verify_no_external_dependencies

echo "External dylib dependencies are bundled."
