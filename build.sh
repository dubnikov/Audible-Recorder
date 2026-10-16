#!/bin/sh
# Cross-compile AudibleRecorder.exe on Linux/macOS/WSL (static, no bundled DLLs).
# Prereqs: mingw-w64, cmake, curl. Run from this directory.
set -e
OPUS=third_party/opus-1.5.2
if [ ! -d $OPUS ]; then
  mkdir -p third_party
  curl -sL https://codeload.github.com/xiph/opus/tar.gz/refs/tags/v1.5.2 | tar xz -C third_party
fi
if [ ! -f $OPUS/build/libopus.a ]; then
cat > /tmp/tc.cmake <<'TC'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
TC
  cmake -S $OPUS -B $OPUS/build -DCMAKE_TOOLCHAIN_FILE=/tmp/tc.cmake -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF
  cmake --build $OPUS/build -j4
fi
x86_64-w64-mingw32-windres app.rc -O coff -o app_res.o
x86_64-w64-mingw32-g++ -O2 -Wall -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -I$OPUS/include main.cpp app_res.o $OPUS/build/libopus.a \
  -luser32 -lgdi32 -lcomdlg32 -lole32 -s -o AudibleRecorder.exe
echo "Built AudibleRecorder.exe"
