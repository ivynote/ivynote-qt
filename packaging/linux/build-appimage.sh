#!/usr/bin/env bash
# Package an existing build into a self-contained AppImage.
#
#   packaging/linux/build-appimage.sh <build-dir> <version> [out-dir]
#
# Needs: qmake of the Qt the app was built against on PATH (or $QMAKE),
# linuxdeploy, linuxdeploy-plugin-qt and appimagetool on PATH (or in $TOOLS_DIR).
# Optional: UPDATE_INFORMATION (e.g. "gh-releases-zsync|owner|repo|latest|Ivynote-*-x86_64.AppImage.zsync")
# embeds update info so AppImageUpdate / Gear Lever can update in place.
set -euo pipefail

BUILD_DIR=$(realpath "${1:?build dir}")
VERSION=${2:?version}
OUT_DIR=$(realpath -m "${3:-.}")
ARCH=${ARCH:-x86_64}
TOOLS_DIR=${TOOLS_DIR:-}
[[ -n $TOOLS_DIR ]] && export PATH="$TOOLS_DIR:$PATH"
for tool in linuxdeploy linuxdeploy-plugin-qt appimagetool; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing $tool. Run 'make appimage-tools' first, or set TOOLS_DIR to your packaging tools directory." >&2
        exit 1
    }
done
# CI runners have no FUSE; the tools are AppImages themselves.
export APPIMAGE_EXTRACT_AND_RUN=1
# appimagetool validates the AppStream metadata with appstreamcli, which
# otherwise fetches every URL in it (and fails while the repo is private).
export AS_VALIDATE_NONET=1
# Bundled strip may predate RELR sections used by modern distributions.
export NO_STRIP=1

APPDIR="$BUILD_DIR/AppDir"
APP_ID=app.ivynote.Ivynote
OUT="$OUT_DIR/Ivynote-$VERSION-$ARCH.AppImage"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr

export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)

# Ship the Wayland platform plugin next to xcb so Wayland sessions run
# natively. Under XWayland the bundled Qt's xcb plugin never receives mouse
# button events on KDE Plasma (the whole window ignores clicks), and native
# Wayland also gets input methods through text-input. Qt prefers "wayland"
# on Wayland sessions and falls back to xcb on X11.
# Qt distributions use either a unified plugin or generic/EGL plugins.
wayland_plugins=()
for name in libqwayland.so libqwayland-generic.so libqwayland-egl.so; do
    [[ -f "$QT_PLUGINS/platforms/$name" ]] && wayland_plugins+=("$name")
done
if [[ ${#wayland_plugins[@]} -eq 0 ]]; then
    echo "No Wayland platform plugin found in $QT_PLUGINS/platforms; install the Wayland plugins for this Qt." >&2
    exit 1
fi
export EXTRA_PLATFORM_PLUGINS=$(IFS=';'; echo "${wayland_plugins[*]}")

# Present a private plugin tree to linuxdeploy instead of modifying system Qt.
# KDE image handlers pull in optional codecs unrelated to this application;
# database drivers other than SQLite similarly require extra client libraries.
PACKAGING_QT="$BUILD_DIR/packaging-qt"
rm -rf "$PACKAGING_QT"
mkdir -p "$PACKAGING_QT/plugins"
cp -as "$QT_PLUGINS"/. "$PACKAGING_QT/plugins/"
find "$PACKAGING_QT/plugins/imageformats" -name 'kimg_*.so' -delete
find "$PACKAGING_QT/plugins/sqldrivers" -name '*.so' ! -name 'libqsqlite.so' -delete
REAL_QMAKE=$(command -v "$QMAKE")
export REAL_QMAKE
export PACKAGING_QT_PLUGINS="$PACKAGING_QT/plugins"
cat > "$PACKAGING_QT/qmake" <<'QMAKE_WRAPPER'
#!/usr/bin/env bash
if [[ ${1:-} == -query && ${2:-} == QT_INSTALL_PLUGINS ]]; then
    printf '%s\n' "$PACKAGING_QT_PLUGINS"
elif [[ ${1:-} == -query && $# -eq 1 ]]; then
    "$REAL_QMAKE" "$@" | while IFS= read -r line; do
        case "$line" in
            QT_INSTALL_PLUGINS:*) printf 'QT_INSTALL_PLUGINS:%s\n' "$PACKAGING_QT_PLUGINS" ;;
            *) printf '%s\n' "$line" ;;
        esac
    done
else
    exec "$REAL_QMAKE" "$@"
fi
QMAKE_WRAPPER
chmod +x "$PACKAGING_QT/qmake"
export QMAKE="$PACKAGING_QT/qmake"

linuxdeploy --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/ivynote" \
    --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
    --plugin qt

# linuxdeploy puts the SVG icon at the AppDir top level and points .DirIcon
# at it, but .DirIcon must be a PNG. Use the PNG for both.
rm -f "${APPDIR:?}/.DirIcon" "${APPDIR:?}/${APP_ID:?}.svg"
cp "$APPDIR/usr/share/icons/hicolor/512x512/apps/$APP_ID.png" "$APPDIR/$APP_ID.png"
ln -s "$APP_ID.png" "$APPDIR/.DirIcon"

# Input-method plugins: ibus ships with Qt, fcitx5 is built into Qt's plugin
# dir by CI. linuxdeploy-plugin-qt normally copies them all; make sure fcitx5
# made it in (most Chinese Linux desktops set QT_IM_MODULE=fcitx).
IMDIR="$APPDIR/usr/plugins/platforminputcontexts"
for so in "$QT_PLUGINS"/platforminputcontexts/*.so; do
    [[ -e $so ]] || continue
    if [[ ! -e "$IMDIR/$(basename "$so")" ]]; then
        echo "adding input-method plugin $(basename "$so")"
        mkdir -p "$IMDIR"
        install -m755 "$so" "$IMDIR/"
        patchelf --set-rpath '$ORIGIN/../../lib' "$IMDIR/$(basename "$so")"
    fi
done
echo "bundled input-method plugins:"; ls "$IMDIR" || true

# Without these a Wayland session falls back to XWayland, where clicks are lost.
for name in "${wayland_plugins[@]}"; do
    [[ -e "$APPDIR/usr/plugins/platforms/$name" ]] || { echo "missing Wayland plugin $name" >&2; exit 1; }
done
for f in wayland-shell-integration/libxdg-shell.so; do
    [[ -e "$APPDIR/usr/plugins/$f" ]] || { echo "missing Wayland plugin $f" >&2; exit 1; }
done

# Execute the real binary so Qt resolves qt.conf beside usr/bin/ivynote.
# A root-level AppRun symlink can make Qt look beside AppRun instead.
rm -f "$APPDIR/AppRun"
cat > "$APPDIR/AppRun" <<'APP_RUN'
#!/bin/sh
set -eu
APPDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export QT_PLUGIN_PATH="$APPDIR/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$APPDIR/usr/plugins/platforms"
exec "$APPDIR/usr/bin/ivynote" "$@"
APP_RUN
chmod +x "$APPDIR/AppRun"

mkdir -p "$OUT_DIR"
rm -f "$OUT" "$OUT.zsync"
args=()
# Allow offline packaging with a previously downloaded type2 runtime.
if [[ -n ${APPIMAGE_RUNTIME_FILE:-} ]]; then
    args+=(--runtime-file "$(realpath "$APPIMAGE_RUNTIME_FILE")")
fi
if [[ -n ${UPDATE_INFORMATION:-} ]]; then
    args+=(-u "$UPDATE_INFORMATION")
fi
# appimagetool's static runtime does not need libfuse2 on the user's machine.
(cd "$OUT_DIR" && VERSION="$VERSION" ARCH="$ARCH" appimagetool "${args[@]}" "$APPDIR" "$OUT")
echo "built $OUT"
