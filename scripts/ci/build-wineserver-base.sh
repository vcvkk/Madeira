#!/bin/bash
# Seed build/wineserver/obj/libwineserver.a from the wine submodule so that
# build/wineserver/build.sh can run on a clean checkout (it only patches an
# existing archive). Every server/*.c that build.sh does NOT recompile itself is
# compiled here with the same flags; build.sh then adds/replaces the rest.
set -eu

R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$R/build/wineserver"
WINE_SRC="$R/wine"
SHIMS_DIR="$R/build/ntdll-unix/shims"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
OBJ_DIR="$BUILD_DIR/obj/base"
OUT="$BUILD_DIR/obj/libwineserver.a"

# Recompiled (or replaced) by build/wineserver/build.sh.
PATCHED=" async class event fd handle inproc_sync mach main mapping object process
          queue region request sock thread unicode user window winstation "

CC_FLAGS=(
    -arch arm64 -isysroot "$SDK" -miphoneos-version-min=17.0 -O2
    -I"$WINE_SRC/include" -I"$WINE_SRC/include/wine"
    -I"$WINE_SRC/build-macos/include"
    -I"$BUILD_DIR" -I"$WINE_SRC/server"
    -I"$SHIMS_DIR"
    -I"$BUILD_DIR/../madsync" -DHAVE_LINUX_NTSYNC_H=1
    -include "$BUILD_DIR/config_ios.h"
    -include stdarg.h
    -include "$BUILD_DIR/unicode_fix.h"
    -include "$BUILD_DIR/wineserver_ios_kill.h"
    -DBINDIR=\"/usr/local/bin\" -DDATADIR=\"/usr/local/share\"
    -D__WINESRC__ -DWINE_IOS=1
    -Dmain=wineserver_main
    -Wno-implicit-function-declaration
)

rm -rf "$OBJ_DIR" && mkdir -p "$OBJ_DIR"
sources=$(sed -n '/^SOURCES/,/^$/p' "$WINE_SRC/server/Makefile.in" | grep -oE '[a-z_0-9]+\.c')
for src in $sources; do
    name="${src%.c}"
    [[ "$PATCHED" == *" $name "* ]] && continue
    echo "  base: $name"
    xcrun -sdk iphoneos clang "${CC_FLAGS[@]}" -c "$WINE_SRC/server/$src" -o "$OBJ_DIR/$name.o"
done
rm -f "$OUT"
ar rcs "$OUT" "$OBJ_DIR"/*.o
echo "Seeded $OUT"
