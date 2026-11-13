#!/bin/sh
# Keep Git detail while satisfying IVY_VERSION's numeric version prefix.
set -eu
base=$(awk '/^project/ {print $3; exit}' CMakeLists.txt)
if description=$(git describe --dirty --always --tags --abbrev=7 2>/dev/null); then
    version=${description#v}
    if printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+'; then
        printf '%s\n' "$version"
    else
        printf '%s-dev.%s\n' "$base" "$description"
    fi
else
    printf '%s\n' "$base"
fi
