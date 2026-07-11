# Cross-compile toolchain: WSL/Linux host -> 32-bit Windows (i686).
#
# 32-bit because the only Windows build of the closed-source pico decoder
# (resources/picodecoder/windows/x86/pico_decoder_1_0_0.dll) is PE32 i386;
# the whole process must match it. Runs fine under WOW64 on Windows 11 x64.
#
# Requires: sudo apt install g++-mingw-w64-i686-posix mingw-w64-tools
# Usage:    cmake --preset win-native && cmake --build build-win-native

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

# Prefer the -posix flavour: winpthreads-backed std::thread/mutex, which the
# C++11 code here expects. The unsuffixed Ubuntu binaries default to the
# win32-threads flavour.
find_program(MINGW_C NAMES i686-w64-mingw32-gcc-posix i686-w64-mingw32-gcc)
find_program(MINGW_CXX NAMES i686-w64-mingw32-g++-posix i686-w64-mingw32-g++)
find_program(MINGW_RC NAMES i686-w64-mingw32-windres)
find_program(MINGW_DLLTOOL NAMES i686-w64-mingw32-dlltool)

if(NOT MINGW_C OR NOT MINGW_CXX)
    message(FATAL_ERROR
        "MinGW i686 cross toolchain not found.\n"
        "Install it with: sudo apt install g++-mingw-w64-i686-posix mingw-w64-tools")
endif()

set(CMAKE_C_COMPILER "${MINGW_C}")
set(CMAKE_CXX_COMPILER "${MINGW_CXX}")
set(CMAKE_RC_COMPILER "${MINGW_RC}")

# Static libgcc/libstdc++/winpthread so the only runtime DLL the exe needs
# beside system libraries is pico_decoder_1_0_0.dll (and its MSVCR90 dep).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# i686 defaults to x87 float; the thread code uses SSE denormal-flush
# intrinsics, and every Windows-11-capable CPU has SSE2
set(CMAKE_C_FLAGS_INIT "-msse2 -mfpmath=sse")
set(CMAKE_CXX_FLAGS_INIT "-msse2 -mfpmath=sse")

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
