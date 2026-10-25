# cmake -DLINKED=a,b -DGUARDED=a,b -DPROFILED=a,b -P app_guard_check.cmake:
# every test binary built on the app (LINKED) is one app_tests_leave_profile_alone
# runs (GUARDED) and one ctest runs under the scratch profile (PROFILED).
# `cmake -P` takes none of the project's policies: set them to its minimum, or
# CMake 3.x reads IN_LIST as a word and fails.
cmake_minimum_required(VERSION 3.24)
foreach(var LINKED GUARDED PROFILED)
  string(REPLACE "," ";" ${var} "${${var}}")
endforeach()
if(NOT LINKED)
  message(FATAL_ERROR "no test binary is built on the app, so there is nothing to check")
endif()
set(fails 0)
foreach(bin IN LISTS LINKED)
  if(NOT bin IN_LIST GUARDED)
    message("FAIL ${bin} is not run by app_tests_leave_profile_alone")
    math(EXPR fails "${fails} + 1")
  endif()
  if(NOT bin IN_LIST PROFILED)
    message("FAIL ${bin} runs under the user's own HOME: give it CROSSCOMPILING_EMULATOR \${BW_APP_TEST_ENV}")
    math(EXPR fails "${fails} + 1")
  endif()
  if(bin IN_LIST GUARDED AND bin IN_LIST PROFILED)
    message("ok   ${bin}")
  endif()
endforeach()
if(fails GREATER 0)
  message(FATAL_ERROR "${fails} gap(s) in the app tests' profile guard")
endif()
