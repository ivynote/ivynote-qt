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
# CI runners have no FUSE; the tools are AppImages themselves.
export APPIMAGE_EXTRACT_AND_RUN=1

APPDIR="$BUILD_DIR/AppDir"
APP_ID=app.ivynote.Ivynote
OUT="$OUT_DIR/Ivynote-$VERSION-$ARCH.AppImage"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr

export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)

linuxdeploy --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/ivynote" \
    --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
    --plugin qt

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

mkdir -p "$OUT_DIR"
rm -f "$OUT" "$OUT.zsync"
args=()
if [[ -n ${UPDATE_INFORMATION:-} ]]; then
    args+=(-u "$UPDATE_INFORMATION")
fi
# appimagetool's static runtime does not need libfuse2 on the user's machine.
(cd "$OUT_DIR" && VERSION="$VERSION" ARCH="$ARCH" appimagetool "${args[@]}" "$APPDIR" "$OUT")
echo "built $OUT"
