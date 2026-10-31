# Fails when a test file names one scratch directory twice. ctest runs each
# TEST_CASE as its own process and scratch() empties its directory first, so
# two cases that share a name delete each other's files when ctest -j runs them
# at once. A case behind #ifdef _WIN32 shares a name with nothing a Linux build
# compiles, so the check reads the sources, not a build.
#   cmake -DSOURCE_DIR=<tests dir> -P scratch_names_check.cmake
cmake_minimum_required(VERSION 3.24)

file(GLOB sources "${SOURCE_DIR}/*.cpp")
set(total 0)
set(clashes "")
foreach(src IN LISTS sources)
  file(READ "${src}" text)
  string(REGEX MATCHALL "scratch\\(\"[^\"]*\"\\)" calls "${text}")
  list(LENGTH calls n)
  math(EXPR total "${total} + ${n}")
  set(seen "")
  foreach(call IN LISTS calls)
    if(call IN_LIST seen)
      get_filename_component(name "${src}" NAME)
      list(APPEND clashes "${name}: ${call}")
    endif()
    list(APPEND seen "${call}")
  endforeach()
endforeach()

if(total EQUAL 0)
  message(FATAL_ERROR "no scratch(\"...\") call found under ${SOURCE_DIR}")
endif()
if(clashes)
  list(REMOVE_DUPLICATES clashes)
  list(JOIN clashes "\n  " lines)
  message(FATAL_ERROR "a scratch directory named twice in one file:\n  ${lines}")
endif()
message(STATUS "${total} scratch directories, none named twice in a file")
