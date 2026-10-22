# CMake toolchain for the Windows cross build (tools/windows-cross-build.sh):
# Arch's mingw-w64-gcc compiles, the MSYS2 UCRT64 tree that
# tools/fetch-msys2-sysroot.py unpacks supplies every library, and the host's
# Qt supplies moc/rcc/uic. Configure with
#   -DCMAKE_TOOLCHAIN_FILE=tools/mingw-w64.cmake -DOPENYOG_SYSROOT=<dir>/ucrt64
# The host Qt (QT_HOST_PATH, default /usr) has to be the sysroot Qt's version.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(OPENYOG_SYSROOT "$ENV{OPENYOG_SYSROOT}" CACHE PATH "MSYS2 UCRT64 tree (…/ucrt64)")
if(NOT IS_DIRECTORY "${OPENYOG_SYSROOT}/lib/cmake/Qt6")
    message(FATAL_ERROR "OPENYOG_SYSROOT=\"${OPENYOG_SYSROOT}\" has no Qt 6: "
                        "run tools/fetch-msys2-sysroot.py and pass its ucrt64/")
endif()
# try_compile() projects re-read this file without the cache; pass it along
set(ENV{OPENYOG_SYSROOT} "${OPENYOG_SYSROOT}")

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# headers, libraries and CMake packages only from the sysroot (then the
# toolchain's own), programs only from the host
set(CMAKE_FIND_ROOT_PATH "${OPENYOG_SYSROOT}" /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(NOT QT_HOST_PATH)
    set(QT_HOST_PATH /usr)
endif()
