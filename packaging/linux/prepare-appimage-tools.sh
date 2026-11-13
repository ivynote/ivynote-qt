#!/usr/bin/env bash
# Download only missing tools; rerun after deleting a tool to refresh it.
set -euo pipefail
TOOLS_DIR=${TOOLS_DIR:-$PWD/tools}
mkdir -p "$TOOLS_DIR"
fetch() {
    local name=$1 url=$2
    [[ -x "$TOOLS_DIR/$name" ]] && return 0
    curl -fL --retry 3 -o "$TOOLS_DIR/$name.tmp" "$url"
    chmod +x "$TOOLS_DIR/$name.tmp"
    mv "$TOOLS_DIR/$name.tmp" "$TOOLS_DIR/$name"
}
base=https://github.com
fetch linuxdeploy "$base/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch linuxdeploy-plugin-qt "$base/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
fetch appimagetool "$base/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
