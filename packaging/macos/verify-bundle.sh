#!/usr/bin/env bash
# Verify a built macOS Ivynote.app bundle and optional DMG:
# - executable and critical Qt plugins exist and have expected architectures
# - Info.plist has the expected bundle ID, icon, version, and minimum macOS
# - no non-system external dylib paths leak into the binary, frameworks, or plugins
# - code signature validates cleanly (and notarization ticket if notarized)
# - DMG mounts cleanly with Ivynote.app and an /Applications symlink
#
# Usage:
#   packaging/macos/verify-bundle.sh --app <Ivynote.app> [--dmg <Ivynote.dmg>] \
#     [--universal] [--version <numeric>] [--min-macos <ver>] [--notarized <true|false>]
set -euo pipefail

APP_PATH=""
DMG_PATH=""
UNIVERSAL=false
EXPECTED_VERSION=""
EXPECTED_MIN_MACOS=""
NOTARIZED=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)
            APP_PATH=$2
            shift 2
            ;;
        --dmg)
            DMG_PATH=$2
            shift 2
            ;;
        --universal)
            UNIVERSAL=true
            shift
            ;;
        --version)
            EXPECTED_VERSION=$2
            shift 2
            ;;
        --min-macos)
            EXPECTED_MIN_MACOS=$2
            shift 2
            ;;
        --notarized)
            NOTARIZED=$2
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$APP_PATH" || ! -d "$APP_PATH" ]]; then
    echo "::error::macOS app bundle not found: ${APP_PATH:-<missing>}" >&2
    exit 1
fi

plist_get() {
    /usr/libexec/PlistBuddy -c "Print :$2" "$1"
}

check_archs() {
    local target=$1
    local archs
    archs=$(lipo -archs "$target")
    echo "Architectures ($target): $archs"
    if [[ "$UNIVERSAL" == true ]]; then
        for arch in arm64 x86_64; do
            if [[ " $archs " != *" $arch "* ]]; then
                echo "::error::Expected universal binary with $arch slice in $target, found: $archs" >&2
                exit 1
            fi
        done
    fi
}

check_otool() {
    local target=$1
    local self_id
    self_id=$(otool -D "$target" | grep -v ':$' | head -n 1 || true)
    local bad=0
    while IFS= read -r line; do
        [[ "$line" == *: ]] && continue
        local dep
        dep=$(printf '%s\n' "$line" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+\(compatibility.*$//')
        [[ -z "$dep" ]] && continue
        [[ -n "$self_id" && "$dep" == "$self_id" ]] && continue
        case "$dep" in
            /System/Library/*|/usr/lib/*|@executable_path/*|@loader_path/*|@rpath/*)
                ;;
            *)
                echo "::error::Disallowed external dylib reference in $target: $dep" >&2
                bad=1
                ;;
        esac
    done < <(otool -L "$target")
    return "$bad"
}

BIN="$APP_PATH/Contents/MacOS/Ivynote"
test -x "$BIN"
check_archs "$BIN"

PLIST="$APP_PATH/Contents/Info.plist"
test -f "$PLIST"

BUNDLE_ID=$(plist_get "$PLIST" CFBundleIdentifier)
SHORT_VER=$(plist_get "$PLIST" CFBundleShortVersionString)
BUILD_VER=$(plist_get "$PLIST" CFBundleVersion)
ICON_FILE=$(plist_get "$PLIST" CFBundleIconFile)
MIN_MACOS=$(plist_get "$PLIST" LSMinimumSystemVersion)

echo "Bundle ID: $BUNDLE_ID, Version: $SHORT_VER ($BUILD_VER), Min macOS: $MIN_MACOS, Icon: $ICON_FILE"

if [[ "$BUNDLE_ID" != "app.ivynote.Ivynote" ]]; then
    echo "::error::Unexpected CFBundleIdentifier: $BUNDLE_ID (expected app.ivynote.Ivynote)" >&2
    exit 1
fi
if [[ "$ICON_FILE" != "app.icns" ]]; then
    echo "::error::Unexpected CFBundleIconFile: $ICON_FILE (expected app.icns)" >&2
    exit 1
fi
if [[ -n "$EXPECTED_VERSION" && "$SHORT_VER" != "$EXPECTED_VERSION" ]]; then
    echo "::error::Unexpected CFBundleShortVersionString: $SHORT_VER (expected $EXPECTED_VERSION)" >&2
    exit 1
fi
if [[ -n "$EXPECTED_VERSION" && "$BUILD_VER" != "$EXPECTED_VERSION" ]]; then
    echo "::error::Unexpected CFBundleVersion: $BUILD_VER (expected $EXPECTED_VERSION)" >&2
    exit 1
fi
if [[ -n "$EXPECTED_MIN_MACOS" && "$MIN_MACOS" != "$EXPECTED_MIN_MACOS" ]]; then
    echo "::error::Unexpected LSMinimumSystemVersion: $MIN_MACOS (expected $EXPECTED_MIN_MACOS)" >&2
    exit 1
fi

test -s "$APP_PATH/Contents/Resources/app.icns"
test -s "$APP_PATH/Contents/Resources/qt.conf"

COCOA_PLUGIN="$APP_PATH/Contents/PlugIns/platforms/libqcocoa.dylib"
SQLITE_PLUGIN="$APP_PATH/Contents/PlugIns/sqldrivers/libqsqlite.dylib"
test -f "$COCOA_PLUGIN"
test -f "$SQLITE_PLUGIN"
check_archs "$COCOA_PLUGIN"
check_archs "$SQLITE_PLUGIN"

EXTRA_SQL=$(find "$APP_PATH/Contents/PlugIns/sqldrivers" -maxdepth 1 -type f -name '*.dylib' ! -name 'libqsqlite.dylib')
if [[ -n "$EXTRA_SQL" ]]; then
    echo "::error::Unexpected extra SQL driver plugins bundled: $EXTRA_SQL" >&2
    exit 1
fi

echo "Auditing linked libraries with otool -L..."
check_otool "$BIN"

if [[ -d "$APP_PATH/Contents/PlugIns" ]]; then
    while IFS= read -r -d '' f; do
        check_otool "$f"
    done < <(find "$APP_PATH/Contents/PlugIns" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
fi

if [[ -d "$APP_PATH/Contents/Frameworks" ]]; then
    while IFS= read -r -d '' f; do
        check_otool "$f"
    done < <(find "$APP_PATH/Contents/Frameworks" -maxdepth 1 -type f \( -name '*.dylib' -o -name '*.so' \) -print0)

    while IFS= read -r -d '' fw; do
        fw_name=$(basename "$fw" .framework)
        fw_bin="$fw/Versions/Current/$fw_name"
        if [[ -f "$fw_bin" ]]; then
            check_otool "$fw_bin"
        fi
    done < <(find "$APP_PATH/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0)
fi

codesign --verify --deep --strict --verbose=2 "$APP_PATH"

if [[ -n "$DMG_PATH" ]]; then
    test -s "$DMG_PATH"
    WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ivynote-verify.XXXXXX")
    MOUNT_DIR="$WORK_DIR/mount"
    MOUNTED=0
    cleanup() {
        if [[ "$MOUNTED" -eq 1 ]]; then
            hdiutil detach "$MOUNT_DIR" -quiet || hdiutil detach "$MOUNT_DIR" -force -quiet || true
        fi
        rm -rf "$WORK_DIR"
    }
    trap cleanup EXIT

    mkdir -p "$MOUNT_DIR"
    hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_DIR" "$DMG_PATH" >/dev/null
    MOUNTED=1

    test -d "$MOUNT_DIR/Ivynote.app"
    test -L "$MOUNT_DIR/Applications"
    codesign --verify --deep --strict "$MOUNT_DIR/Ivynote.app"

    hdiutil detach "$MOUNT_DIR" -quiet || hdiutil detach "$MOUNT_DIR" -force -quiet
    MOUNTED=0

    if [[ "$NOTARIZED" == "true" ]]; then
        xcrun stapler validate "$DMG_PATH"
    fi
fi

echo "macOS verification passed: $APP_PATH"
