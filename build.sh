#!/usr/bin/env bash
# Builds the 32-bit proxy xinput1_3.dll with zig (clang + lld, mingw target).
#   bash build.sh        release build: build/xinput1_3.dll (no dev tooling, no symbols, version resource)
#   bash build.sh dev    developer build: build/dev/xinput1_3.dll (dev server, shared-memory bridge with a virtual pad
#                        and screenshots, active with RE3C_dev.flag next to the DLL; used by tools/*)
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"  # zig 0.13+ on PATH, or ZIG=/path/to/zig.exe
MODE="${1:-release}"
mkdir -p build/dev
MH=third_party/minhook/src
"$ZIG" cc -target x86-windows-gnu -O2 -c $MH/hook.c -o build/mh_hook.o -Ithird_party/minhook/include
"$ZIG" cc -target x86-windows-gnu -O2 -c $MH/buffer.c -o build/mh_buffer.o -Ithird_party/minhook/include
"$ZIG" cc -target x86-windows-gnu -O2 -c $MH/trampoline.c -o build/mh_trampoline.o -Ithird_party/minhook/include
"$ZIG" cc -target x86-windows-gnu -O2 -c $MH/hde/hde32.c -o build/mh_hde32.o -Ithird_party/minhook/include
FLAGS=(-target x86-windows-gnu -O2 -std=c++17 -shared -fno-exceptions -fno-rtti -Ithird_party/minhook/include -Isrc)
if [ "$MODE" = dev ]; then
  "$ZIG" c++ "${FLAGS[@]}" -DRE3C_DEV=1 src/*.cpp build/mh_*.o src/xinput1_3.def src/version.rc \
    -o build/dev/xinput1_3.dll -luser32 -lkernel32 -lws2_32
  ls -la build/dev/xinput1_3.dll
else
  SRCS=$(ls src/*.cpp | grep -v -e devserver.cpp -e d3d9_hook.cpp)
  "$ZIG" c++ "${FLAGS[@]}" -g0 -s $SRCS build/mh_*.o src/xinput1_3.def src/version.rc \
    -o build/xinput1_3.dll -luser32 -lkernel32
  ls -la build/xinput1_3.dll
fi
