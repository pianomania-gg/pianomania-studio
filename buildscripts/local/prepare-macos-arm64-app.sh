#!/usr/bin/env bash

set -euo pipefail

usage() {
    printf 'Usage: prepare-macos-arm64-app.sh <application-bundle>\n' >&2
}

if [[ "$#" -ne 1 ]]; then
    usage
    exit 2
fi

APP_PATH="$1"
CONTENTS_PATH="${APP_PATH}/Contents"

if [[ ! -d "$CONTENTS_PATH" || -L "$APP_PATH" ]]; then
    printf 'Application bundle is missing or invalid: %s\n' "$APP_PATH" >&2
    exit 1
fi

for required_tool in chmod file find lipo mktemp mv rm stat xargs; do
    if ! command -v "$required_tool" >/dev/null 2>&1; then
        printf 'Required tool is missing: %s\n' "$required_tool" >&2
        exit 1
    fi
done

thin_mach_o_to_arm64() {
    local target_path="$1"
    local architectures
    local original_mode
    local temporary_path

    architectures="$(lipo -archs "$target_path" | xargs)"
    if [[ ! " ${architectures} " =~ [[:space:]]arm64[[:space:]] ]]; then
        printf 'Mach-O file lacks required arm64 architecture: %s (%s)\n' \
            "$target_path" "$architectures" >&2
        return 1
    fi

    if [[ "$architectures" == "arm64" ]]; then
        return 0
    fi

    original_mode="$(stat -f %Lp "$target_path")"
    temporary_path="$(mktemp "${target_path}.pianomania-arm64.XXXXXX")"
    if ! lipo "$target_path" -thin arm64 -output "$temporary_path"; then
        rm -f "$temporary_path"
        return 1
    fi
    chmod "$original_mode" "$temporary_path"
    mv -f "$temporary_path" "$target_path"

    [[ "$(lipo -archs "$target_path" | xargs)" == "arm64" ]] || {
        printf 'Mach-O file did not become arm64-only: %s\n' "$target_path" >&2
        return 1
    }
}

find "$CONTENTS_PATH" -type d -name '*.dSYM' -prune -exec rm -rf {} +

manifest_path="$(mktemp "${TMPDIR:-/tmp}/pianomania-arm64-manifest.XXXXXX")"
cleanup() {
    rm -f "$manifest_path"
}
trap cleanup EXIT

write_manifest() {
    if ! find "$CONTENTS_PATH" -type f -print0 >"$manifest_path"; then
        printf 'Could not enumerate every application bundle file: %s\n' "$APP_PATH" >&2
        exit 1
    fi
}

mach_o_count=0
write_manifest
while IFS= read -r -d '' target_path; do
    if ! file_description="$(file -E -b "$target_path")"; then
        printf 'Could not classify application bundle file: %s\n' "$target_path" >&2
        exit 1
    fi
    if [[ "$file_description" == *Mach-O* ]]; then
        thin_mach_o_to_arm64 "$target_path"
        mach_o_count=$((mach_o_count + 1))
    fi
done <"$manifest_path"

if [[ "$mach_o_count" -eq 0 ]]; then
    printf 'Application bundle contains no Mach-O code: %s\n' "$APP_PATH" >&2
    exit 1
fi

audited_mach_o_count=0
write_manifest
while IFS= read -r -d '' target_path; do
    if ! file_description="$(file -E -b "$target_path")"; then
        printf 'Could not classify application bundle file during final audit: %s\n' \
            "$target_path" >&2
        exit 1
    fi
    if [[ "$file_description" == *Mach-O* ]]; then
        architectures="$(lipo -archs "$target_path" | xargs)"
        if [[ "$architectures" != "arm64" ]]; then
            printf 'Final architecture audit found non-arm64 code: %s (%s)\n' \
                "$target_path" "$architectures" >&2
            exit 1
        fi
        audited_mach_o_count=$((audited_mach_o_count + 1))
    fi
done <"$manifest_path"

if [[ "$audited_mach_o_count" -ne "$mach_o_count" ]]; then
    printf 'Final architecture audit count differs: prepared %s, audited %s.\n' \
        "$mach_o_count" "$audited_mach_o_count" >&2
    exit 1
fi

printf 'Prepared %s arm64-only Mach-O files.\n' "$mach_o_count"
