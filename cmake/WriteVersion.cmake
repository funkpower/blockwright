# Writes version.h with the project version and short git commit; rewrites
# it only when the text changes, so an unchanged commit doesn't relink.
#   cmake -DSRC=<source dir> -DOUT=<header> -DVERSION=<x.y.z> [-DCOMMIT=<sha>] -P WriteVersion.cmake
if(NOT COMMIT)
  execute_process(COMMAND git -C "${SRC}" rev-parse --short HEAD
                  OUTPUT_VARIABLE COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0 OR NOT COMMIT)
    set(COMMIT "unknown")
  else()
    execute_process(COMMAND git -C "${SRC}" diff --quiet HEAD --
                    RESULT_VARIABLE dirty ERROR_QUIET)
    if(dirty EQUAL 1)
      string(APPEND COMMIT "-dirty")
    endif()
  endif()
endif()

set(text "#pragma once\n#define BLOCKWRIGHT_VERSION \"${VERSION}\"\n#define BLOCKWRIGHT_GIT_COMMIT \"${COMMIT}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
else()
  set(old "")
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
