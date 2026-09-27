#!/bin/bash
# Configure the wine submodule for its host-side headers (wine/build-macos,
# symlinked as wine/build-arm64ec). The unix-side build scripts only need
# build-macos/include (config.h and the generated headers).
#
# Usage: scripts/ci/configure-wine.sh            source edits + configure
#        scripts/ci/configure-wine.sh --sources  source edits only (used when
#                                                build-macos came from cache)
set -eux

R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
W="$R/wine"

if [ ! -f "$W/dlls/ntdll/arm64ec_x64_export_iat.c" ]; then
    printf '%s\n' '/* generated during PE build; empty for host configure */' \
        > "$W/dlls/ntdll/arm64ec_x64_export_iat.c"
fi

# The fork includes build/madeira_cfg.h ahead of config.h on purpose
# (ml1122); makedep's include-order sanity check rejects that and configure
# then writes no Makefile. Drop the check in CI only.
sed -i '' 's/fatal_error( "%s must be included before other headers\\n", file->name );/;/' "$W/tools/makedep.c"
! grep -q 'must be included before other headers\\n", file->name' "$W/tools/makedep.c"

ln -sfn build-macos "$W/build-arm64ec"
[ "${1:-}" = "--sources" ] && { test -f "$W/build-macos/include/config.h"; exit 0; }

mkdir -p "$W/build-macos" && cd "$W/build-macos"
set +e
../configure --without-x --without-freetype --disable-tests --with-mingw=clang \
    > "$R/wine-configure.log" 2>&1
set -e
tail -40 "$R/wine-configure.log" || true
test -f include/config.h

if [ -f Makefile ]; then
    make -j"$(sysctl -n hw.ncpu)" include/all || make -C include all || true
else
    echo "::warning::Wine Makefile missing (makedep exit); using config.h only"
    mkdir -p include
    touch include/stamp-h
fi
ls -la "$W/build-macos/include/config.h"
