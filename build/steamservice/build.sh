#!/bin/bash
# Build the stand-in for Steam's 32-bit SteamService.exe /installscript (see
# steamservice.c):
#   steamservice-arm64.exe -> app/Madeira/aarch64-windows/ (shipped; the session
#                             links it into system32)
#   steamservice-x64.exe   -> build/steamservice/ only, for test.sh under a
#                             desktop x86-64 Wine (same source)
# The shipped one is native ARM64 on purpose: an x64 child process that exits
# leaves its private ntdll/FEX mappings behind, and the next x64 child -- the
# game Steam starts right after the script -- then fails to load FEX.
#
# Usage: build/steamservice/build.sh [llvm-mingw bin dir]
#   default: $LLVM_MINGW/bin, else toolchains/llvm-mingw-*/bin, else PATH
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

CFLAGS="-O2 -municode -mwindows -Wall -Wextra -Wno-unused-parameter"
LIBS="-lshell32 -ladvapi32"

"${BIN}aarch64-w64-mingw32-clang" $CFLAGS -o "$DIR/steamservice-arm64.exe" "$DIR/steamservice.c" $LIBS
"${BIN}x86_64-w64-mingw32-clang" $CFLAGS -o "$DIR/steamservice-x64.exe" "$DIR/steamservice.c" $LIBS
"${BIN}llvm-strip" "$DIR/steamservice-arm64.exe" "$DIR/steamservice-x64.exe"

cp "$DIR/steamservice-arm64.exe" "$REPO_ROOT/app/Madeira/aarch64-windows/steamservice-arm64.exe"
ls -l "$REPO_ROOT/app/Madeira/aarch64-windows/steamservice-arm64.exe" "$DIR/steamservice-x64.exe"
