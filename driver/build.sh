#!/usr/bin/env bash
# Cross-build the Windows binaries on Linux.
#   sudo apt install g++-mingw-w64-x86-64 g++-mingw-w64-i686 zip
set -euo pipefail
cd "$(dirname "$0")"
VERSION="${1:-dev}"
mkdir -p build
FLAGS="-std=gnu++17 -O2 -Wall -static -static-libgcc -static-libstdc++"
for arch in x86_64 i686; do
  bits=$([ "$arch" = x86_64 ] && echo 64 || echo 32)
  "$arch-w64-mingw32-g++" $FLAGS -shared -o "build/afbffb$bits.dll" src/afbffb.cpp src/afbffb.def \
    -Wl,--kill-at,--enable-stdcall-fixup -lole32 -luuid -lhid -ladvapi32
done
x86_64-w64-mingw32-g++ $FLAGS -municode -o build/fftest.exe tools/fftest.cpp \
  -ldinput8 -ldxguid -lole32 -luuid -luser32
cp scripts/*.cmd build/
(cd build && rm -f afbffb-*.zip && zip -q "afbffb-$VERSION.zip" afbffb64.dll afbffb32.dll fftest.exe install.cmd uninstall.cmd)
ls -la build
