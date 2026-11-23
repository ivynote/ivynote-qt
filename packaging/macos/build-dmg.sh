#!/usr/bin/env bash
# Deploy Qt frameworks/plugins into a built Ivynote.app bundle, sign it
# inside-out (Developer ID or ad-hoc), and package a compressed DMG with an
# /Applications symlink.
#
#   packaging/macos/build-dmg.sh <build-dir> <version> [out-dir]
#
# Optional environment variables:
#   ARCH           Filename arch suffix (default: auto-detected from binary)
#   MACDEPLOYQT    Path to macdeployqt (default: $QT_ROOT_DIR/bin/macdeployqt or PATH)
#   SIGN_IDENTITY  Code signing identity (default: "-" for ad-hoc signing)
#   STAGE_APP_OUT  Optional destination path to keep a copy of the final .app
set -euo pipefail

BUILD_DIR=$(cd "${1:?build dir}" && pwd)
VERSION=${2:?version}
OUT_DIR=${3:-.}
mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)

APP_SRC="$BUILD_DIR/Ivynote.app"
if [[ ! -d "$APP_SRC" ]]; then
    echo "App bundle not found: $APP_SRC" >&2
    exit 1
fi

if [[ -n "${MACDEPLOYQT:-}" ]]; then
    DEPLOY_BIN=$MACDEPLOYQT
elif [[ -n "${QT_ROOT_DIR:-}" && -x "$QT_ROOT_DIR/bin/macdeployqt" ]]; then
    DEPLOY_BIN="$QT_ROOT_DIR/bin/macdeployqt"
elif command -v macdeployqt >/dev/null 2>&1; then
    DEPLOY_BIN=$(command -v macdeployqt)
else
    echo "Missing macdeployqt. Add Qt's bin directory to PATH or set QT_ROOT_DIR / MACDEPLOYQT." >&2
    exit 1
fi

STAGE_DIR="$BUILD_DIR/stage"
STAGE_APP="$STAGE_DIR/Ivynote.app"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
ditto "$APP_SRC" "$STAGE_APP"

echo "Running macdeployqt: $DEPLOY_BIN"
"$DEPLOY_BIN" "$STAGE_APP" -verbose=1

# Keep only the SQLite driver: other Qt SQL drivers link against client libraries
# we do not ship, matching Linux and Windows packaging.
SQL_DIR="$STAGE_APP/Contents/PlugIns/sqldrivers"
if [[ -d "$SQL_DIR" ]]; then
    find "$SQL_DIR" -maxdepth 1 -type f -name '*.dylib' ! -name 'libqsqlite.dylib' -delete
fi
if [[ ! -f "$SQL_DIR/libqsqlite.dylib" ]]; then
    echo "Missing required SQLite plugin: $SQL_DIR/libqsqlite.dylib" >&2
    exit 1
fi

# Exclude unused positioning plugins if deployed.
rm -rf "$STAGE_APP/Contents/PlugIns/position"

if [[ ! -f "$STAGE_APP/Contents/PlugIns/platforms/libqcocoa.dylib" ]]; then
    echo "Missing required Cocoa platform plugin in $STAGE_APP/Contents/PlugIns/platforms" >&2
    exit 1
fi

# Re-sign everything inside-out because macdeployqt rewrites Mach-O load commands.
SIGN_IDENTITY=${SIGN_IDENTITY:--}

sign_item() {
    local target=$1
    if [[ "$SIGN_IDENTITY" != "-" ]]; then
        codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" "$target"
    else
        codesign --force --sign - "$target"
    fi
}

echo "Signing bundle inside-out (identity: $SIGN_IDENTITY)..."
if [[ -d "$STAGE_APP/Contents/PlugIns" ]]; then
    while IFS= read -r -d '' f; do
        sign_item "$f"
    done < <(find "$STAGE_APP/Contents/PlugIns" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
fi

if [[ -d "$STAGE_APP/Contents/Frameworks" ]]; then
    while IFS= read -r -d '' f; do
        sign_item "$f"
    done < <(find "$STAGE_APP/Contents/Frameworks" -maxdepth 1 -type f \( -name '*.dylib' -o -name '*.so' \) -print0)

    while IFS= read -r -d '' fw; do
        sign_item "$fw"
    done < <(find "$STAGE_APP/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0)
fi

sign_item "$STAGE_APP"
codesign --verify --deep --strict --verbose=2 "$STAGE_APP"

if [[ -z "${ARCH:-}" ]]; then
    BIN_ARCHS=$(lipo -archs "$STAGE_APP/Contents/MacOS/Ivynote")
    if [[ " $BIN_ARCHS " == *" arm64 "* && " $BIN_ARCHS " == *" x86_64 "* ]]; then
        ARCH=universal
    elif [[ " $BIN_ARCHS " == *" arm64 "* ]]; then
        ARCH=arm64
    elif [[ " $BIN_ARCHS " == *" x86_64 "* ]]; then
        ARCH=x64
    else
        ARCH=$(uname -m)
    fi
fi

OUT="$OUT_DIR/Ivynote-$VERSION-macos-$ARCH.dmg"
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ivynote-dmg.XXXXXX")
cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

DMG_STAGE="$WORK_DIR/dmg"
mkdir -p "$DMG_STAGE"
# ditto preserves symlinks, extended attributes, and code signatures.
ditto "$STAGE_APP" "$DMG_STAGE/Ivynote.app"
ln -s /Applications "$DMG_STAGE/Applications"

rm -f "$OUT"

# GitHub Actions macOS runners occasionally hit transient "Resource busy" errors
# from Spotlight indexing the temporary image; retry a few times.
attempt=1
max_attempts=3
while true; do
    if hdiutil create \
        -volname "Ivynote" \
        -srcfolder "$DMG_STAGE" \
        -ov \
        -format UDZO \
        -imagekey zlib-level=9 \
        "$OUT"; then
        break
    fi
    if [[ "$attempt" -ge "$max_attempts" ]]; then
        echo "hdiutil create failed after $max_attempts attempts" >&2
        exit 1
    fi
    echo "hdiutil create failed (attempt $attempt/$max_attempts); retrying in 5s..." >&2
    rm -f "$OUT"
    sleep 5
    attempt=$((attempt + 1))
done

if [[ "$SIGN_IDENTITY" != "-" ]]; then
    codesign --force --timestamp --sign "$SIGN_IDENTITY" "$OUT"
fi

if [[ -n "${STAGE_APP_OUT:-}" ]]; then
    rm -rf "$STAGE_APP_OUT"
    mkdir -p "$(dirname "$STAGE_APP_OUT")"
    ditto "$STAGE_APP" "$STAGE_APP_OUT"
fi

echo "Built $OUT ($(du -h "$OUT" | awk '{print $1}'))"
