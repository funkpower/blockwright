# Catches the registrations a contributor could write by mistake: a binary named
# by absolute path, relative path or file name, bare or in a shell string. It
# does not try to defeat deliberate evasion (a renamed copy, a path built at run time).
# It reads the command lines ctest registers, not the files those commands run:
# a test whose script file runs an xcb binary (bash launch-check.sh) is out of
# its reach, so that test has to run through xcb_test.sh itself, and review,
# not this guard, holds it to that.
#
# cmake -DLINKED=a,b -DGUARDED=a,b -DPROFILED=a,b -DXCB=c -DFILE_c=<path>
#       -DXCB_ENV=bash|xcb_test.sh|<dir> -DCTEST=<ctest> -DTEST_DIR=<dir> -DCONFIG=<config>
#       -P app_guard_check.cmake:
# every test binary built on the app (LINKED) is one whose profile is checked:
# app_tests_leave_profile_alone runs it (GUARDED), or xcb_test.sh does; and one
# ctest runs under a scratch profile (PROFILED), or xcb_test.sh's.
# The XCB binaries, built at FILE_<bin>, have no scratch profile emulator. One
# of them counts as both when a test runs it through XCB_ENV and no other test
# names it. The commands are ctest's own for the tests in TEST_DIR
# (--show-only=json-v1), as it will run them: a test that names the binary by
# $<TARGET_FILE>, or a CMP0158 that drops its emulator, shows there. The guard
# doesn't parse shell, so it fails closed: any argument that names the binary's
# file as a whole name (see _names_file) counts as a run of it, and as a bare
# one unless the command starts with XCB_ENV. A mention that runs nothing
# (backend=xcb, echo /build/xcb/data) fails the guard too, on purpose: a test
# that has to name the binary runs through XCB_ENV, and any other test doesn't
# name it. A test whose executable isn't built has no command there, so what it
# runs can't be read: the guard fails until the tree is built.
# `cmake -P` takes none of the project's policies: set them to its minimum, or
# CMake 3.x reads IN_LIST as a word and fails.
cmake_minimum_required(VERSION 3.24)
foreach(var LINKED GUARDED PROFILED XCB)
  string(REPLACE "," ";" ${var} "${${var}}")
endforeach()
if(NOT LINKED)
  message(FATAL_ERROR "no test binary is built on the app, so there is nothing to check")
endif()
set(fails 0)

# An argument names a binary when the binary's file name is in it as a whole
# name: at the start of the argument or right after a character that can't be
# part of a file name, and at the end of the argument or right before one. The
# file-name characters are POSIX's portable set (letters, digits, `.`, `_` and
# `-`); every other character, `/`, `=`, quotes and whitespace among them, is a
# boundary. So a path that ends in the name counts (/dir/xcb, ./xcb), and so do
# --exe=xcb, backend='xcb' and /build/xcb/data; a longer name that only
# contains it doesn't (not-xcb, xcb-tests, xcb_test.sh, /build/xcb-data/).
function(_names_file arg name out)
  string(LENGTH "${name}" len)
  set(${out} OFF PARENT_SCOPE)
  set(before "")
  while(1)
    string(FIND "${arg}" "${name}" at)
    if(at EQUAL -1)
      return()
    endif()
    if(at GREATER 0)
      math(EXPR b "${at} - 1")
      string(SUBSTRING "${arg}" ${b} 1 before)
    endif()
    math(EXPR after "${at} + ${len}")
    string(SUBSTRING "${arg}" ${after} 1 next)
    if(NOT before MATCHES "^[A-Za-z0-9._-]$" AND NOT next MATCHES "^[A-Za-z0-9._-]$")
      set(${out} ON PARENT_SCOPE)
      return()
    endif()
    string(SUBSTRING "${arg}" ${at} 1 before)
    math(EXPR at "${at} + 1")
    string(SUBSTRING "${arg}" ${at} -1 arg)
  endwhile()
endfunction()

if(XCB AND XCB_ENV)
  string(REPLACE "|" ";" XCB_ENV "${XCB_ENV}")
  list(LENGTH XCB_ENV wrap_len)
  # ctest gives the full path of a command it found on PATH (bash), so the
  # first word is matched by name.
  list(POP_FRONT XCB_ENV wrap_cmd)
  get_filename_component(wrap_cmd "${wrap_cmd}" NAME)
  set(config_args)
  if(NOT CONFIG STREQUAL "")
    set(config_args -C ${CONFIG})
  endif()
  execute_process(COMMAND ${CTEST} --test-dir ${TEST_DIR} ${config_args} --show-only=json-v1
    OUTPUT_VARIABLE json ERROR_VARIABLE err RESULT_VARIABLE st)
  if(NOT st EQUAL 0)
    message(FATAL_ERROR "ctest could not list the tests in ${TEST_DIR} (exit ${st}): ${err}")
  endif()
  foreach(bin IN LISTS XCB)
    get_filename_component(name_${bin} "${FILE_${bin}}" NAME)
    set(wrapped_${bin} 0)
    set(bare_${bin} 0)
  endforeach()
  string(JSON ntests LENGTH "${json}" tests)
  if(ntests GREATER 0)
    math(EXPR last "${ntests} - 1")
    foreach(i RANGE ${last})
      # One read of the whole list per test; the rest reads the test alone.
      string(JSON test GET "${json}" tests ${i})
      string(JSON name GET "${test}" name)
      string(JSON command ERROR_VARIABLE no_command GET "${test}" command)
      if(no_command OR command STREQUAL "" OR command STREQUAL "[]")
        message("FAIL the test ${name} has no command: its executable isn't built, so what it runs "
                "can't be checked; build the tree, then run the guard")
        math(EXPR fails "${fails} + 1")
        continue()
      endif()
      string(JSON nargs LENGTH "${command}")
      math(EXPR lastarg "${nargs} - 1")
      # This guard's own test names each binary in its arguments; it runs none.
      string(JSON script GET "${command}" ${lastarg})
      if(script STREQUAL CMAKE_CURRENT_LIST_FILE)
        continue()
      endif()
      # through: the command starts with XCB_ENV, so whatever follows runs in
      # xcb_test.sh's profile
      set(through OFF)
      if(nargs GREATER wrap_len)
        string(JSON first GET "${command}" 0)
        get_filename_component(first "${first}" NAME)
        if(first STREQUAL wrap_cmd)
          set(through ON)
          set(k 1)
          foreach(want IN LISTS XCB_ENV)
            string(JSON have GET "${command}" ${k})
            if(NOT have STREQUAL want)
              set(through OFF)
            endif()
            math(EXPR k "${k} + 1")
          endforeach()
        endif()
      endif()
      foreach(j RANGE ${lastarg})
        string(JSON arg GET "${command}" ${j})
        foreach(bin IN LISTS XCB)
          _names_file("${arg}" "${name_${bin}}" named)
          if(named)
            if(through AND NOT j LESS wrap_len)
              math(EXPR wrapped_${bin} "${wrapped_${bin}} + 1")
            else()
              math(EXPR bare_${bin} "${bare_${bin}} + 1")
              message("FAIL ${bin} is named outside xcb_test.sh in the test ${name}: a test that names it "
                      "runs through xcb_test.sh (BW_APP_XCB_TEST_ENV), or doesn't name it")
              math(EXPR fails "${fails} + 1")
            endif()
          endif()
        endforeach()
      endforeach()
    endforeach()
  endif()
  foreach(bin IN LISTS XCB)
    if(wrapped_${bin} GREATER 0 AND bare_${bin} EQUAL 0)
      list(APPEND GUARDED ${bin})
      list(APPEND PROFILED ${bin})
    endif()
  endforeach()
endif()

foreach(bin IN LISTS LINKED)
  if(NOT bin IN_LIST GUARDED)
    message("FAIL ${bin} is not run by app_tests_leave_profile_alone, nor only through xcb_test.sh")
    math(EXPR fails "${fails} + 1")
  endif()
  if(NOT bin IN_LIST PROFILED)
    message("FAIL ${bin} runs under the user's own HOME: give it CROSSCOMPILING_EMULATOR \${BW_APP_TEST_RUNNER}, "
            "or if it needs an X server run it with add_test(COMMAND \${BW_APP_XCB_TEST_ENV} $<TARGET_FILE:...>)")
    math(EXPR fails "${fails} + 1")
  endif()
  if(bin IN_LIST GUARDED AND bin IN_LIST PROFILED)
    message("ok   ${bin}")
  endif()
endforeach()
if(fails GREATER 0)
  message(FATAL_ERROR "${fails} gap(s) in the app tests' profile guard")
endif()
