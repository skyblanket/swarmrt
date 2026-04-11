# MinGW-w64 cross-compilation toolchain for SwarmRT
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -B build-win ..
#   cmake --build build-win
#
# Prerequisites:
#   brew install mingw-w64    (macOS)
#   apt install mingw-w64     (Ubuntu/Debian)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_ASM_COMPILER x86_64-w64-mingw32-gcc)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
