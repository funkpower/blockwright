# Cross-compiles for 64-bit Windows with llvm-mingw (clang, libc++, UCRT):
# https://github.com/mstorsjo/llvm-mingw
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x86_64.cmake \
#         -DLLVM_MINGW=<llvm-mingw dir> -DCMAKE_PREFIX_PATH="<Qt win64_llvm_mingw>;<deps>" \
#         -DQT_HOST_PATH=<Qt linux_gcc_64 of the same version> ...
#
# LLVM_MINGW may also come from the environment; without it the compilers are
# looked up on PATH. packaging/windows/README.md sets the toolchain up.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT LLVM_MINGW AND DEFINED ENV{LLVM_MINGW})
  set(LLVM_MINGW "$ENV{LLVM_MINGW}")
endif()
set(LLVM_MINGW "${LLVM_MINGW}" CACHE PATH "llvm-mingw toolchain directory")
# try_compile projects see this file again; hand them the directory.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW)

set(_bw_triple x86_64-w64-mingw32)
if(LLVM_MINGW)
  set(_bw_bin "${LLVM_MINGW}/bin/")
  set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/${_bw_triple}")
else()
  set(_bw_bin "")
endif()
set(CMAKE_C_COMPILER "${_bw_bin}${_bw_triple}-clang")
set(CMAKE_CXX_COMPILER "${_bw_bin}${_bw_triple}-clang++")
set(CMAKE_RC_COMPILER "${_bw_bin}${_bw_triple}-windres")
set(CMAKE_AR "${_bw_bin}llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_bw_bin}llvm-ranlib" CACHE FILEPATH "")

# Libraries and headers come from the target's prefixes (CMAKE_PREFIX_PATH is
# searched under the root as given), programs from the host.
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
