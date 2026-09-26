#!/bin/bash
# Configure (first time) and build the ARM64EC FEX module (libarm64ecfex.dll,
# shipped as xtajit64.dll). Options mirror the development build: with them the
# FEX submodule at ml908 reproduced the shipped DLL's size, SizeOfImage, imports
# and exports exactly (llvm-mingw 20260421; the bytes differ only by embedded
# build paths and __DATE__). FEX_IOS_HOST must reach C, C++ and the assembler
# (Module.S picks the unprefixed SyncThreadContext symbol with it), and LTO must
# be off or the EC link loses libc++.
#
# patches/fex-*.patch carry FEX fixes that are not in the fork yet; they are
# applied here if the submodule does not already contain them.
# LLVM_MINGW overrides the toolchain directory (e.g. the Linux release).
set -eu
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PATH="${LLVM_MINGW:-$R/toolchains/llvm-mingw-20260421-ucrt-macos-universal}/bin:$PATH"
B="$R/FEX/build-arm64ec"
for p in "$R"/patches/fex-*.patch; do
    [ -e "$p" ] || continue
    if git -C "$R/FEX" apply --reverse --check "$p" 2>/dev/null; then
        echo "already applied: $(basename "$p")"
    else
        git -C "$R/FEX" apply "$p" && echo "applied: $(basename "$p")"
    fi
done
if [ ! -f "$B/CMakeCache.txt" ]; then
    cmake -S "$R/FEX" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$R/FEX/Data/CMake/toolchain_mingw.cmake" \
        -DMINGW_TRIPLE=arm64ec-w64-mingw32 -DFEX_IOS_HOST_BUILD=ON \
        -DCMAKE_C_FLAGS=-DFEX_IOS_HOST -DCMAKE_CXX_FLAGS=-DFEX_IOS_HOST -DCMAKE_ASM_FLAGS=-DFEX_IOS_HOST \
        -DENABLE_LTO=OFF -DENABLE_FEX_ALLOCATOR=ON -DENABLE_JEMALLOC_GLIBC_ALLOC=ON \
        -DBUILD_FEXCONFIG=ON -DENABLE_CLANG_THUNKS=ON -DENABLE_CCACHE=OFF \
        -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DENABLE_ASSERTIONS=OFF
fi
cmake --build "$B" --target arm64ecfex
cp "$B/Bin/libarm64ecfex.dll" "$R/app/Madeira/arm64ec-windows/xtajit64.dll" && ls -l "$R/app/Madeira/arm64ec-windows/xtajit64.dll"
