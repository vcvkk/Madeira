#!/bin/bash
# Build steamservice-x64.exe (the 64-bit stand-in for Steam's 32-bit
# SteamService.exe /installscript, see steamservice.c) and copy it into the
# arm64ec-windows bundle, where the Steam session links it into system32.
#
# Usage: build/steamservice/build.sh [llvm-mingw bin dir]
#   default: $LLVM_MINGW/bin, else toolchains/llvm-mingw-*/bin, else PATH
# Test on a Linux/macOS box with Wine: build/steamservice/test.sh
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"

if [ $# -ge 1 ]; then
    BIN="$1/"
elif [ -n "${LLVM_MINGW:-}" ]; then
    BIN="$LLVM_MINGW/bin/"
else
    BIN="$(ls -d "$REPO_ROOT"/toolchains/llvm-mingw-*/bin 2>/dev/null | head -1)"
    BIN="${BIN:+$BIN/}"
fi

"${BIN}x86_64-w64-mingw32-clang" -O2 -municode -mwindows -Wall -Wextra -Wno-unused-parameter \
    -o "$DIR/steamservice-x64.exe" "$DIR/steamservice.c" -lshell32 -ladvapi32
"${BIN}llvm-strip" "$DIR/steamservice-x64.exe"

cp "$DIR/steamservice-x64.exe" "$REPO_ROOT/app/Madeira/arm64ec-windows/steamservice-x64.exe"
ls -l "$REPO_ROOT/app/Madeira/arm64ec-windows/steamservice-x64.exe"
