# A project laid out like tests/: app_guard_fixture_test.sh configures it and
# runs its app_tests_all_guarded. With LATE on, an app test binary is declared
# after bw_add_app_guard_test(), another in a subdirectory, and a third is put
# in BW_APP_TEST_BINARIES only after the call, too late for the profile check
# to have run it; the guard has to reject all three. An app test on an X server
# passes when its test runs it through the xcb wrapper, and with LATE one named
# as the real xcb binary has no test at all; the guard has to reject it too,
# whatever its name. With XCB_BAD, three more have the wrapper only as their
# CROSSCOMPILING_EMULATOR: one test runs its binary by $<TARGET_FILE>, which
# CMake never wraps, and another by target name, which it wraps only under
# CMP0158 OLD; a third binary has a wrapped test and also a bare one. The guard
# has to reject each one whose test runs it bare. With XCB_SHELL a test runs the
# wrapped xcb binary from a shell command string, and with XCB_BARE one runs it
# by $<TARGET_FILE>, which before a build has no command for the guard to read;
# the guard has to reject both. With XCB_RELATIVE a shell command string runs
# the xcb binary by a path relative to its working directory, which the guard
# has to reject, and one runs not-xcb that way, which isn't a run of xcb. With
# XCB_OPTION one test hands the xcb binary's path to a shell as an option's
# value, two hand it only the name after --exe= and have the shell strip the
# prefix off (running ./xcb, and xcb found on PATH=.), and one runs it by its
# bare name; the guard has to reject all four. With XCB_MENTION four tests only
# mention xcb as a whole name (inside backend=xcb, quoted, as a directory in a
# path, and as a quoted shell assignment), which the guard rejects by design:
# it can't tell a mention from a run. With XCB_SUBSTRING tests print
# longer names that contain xcb (/build/xcb-data/, libxcb.so, xcb_test.sh, and
# xcb next to each file-name character class on each side: an uppercase and a
# lowercase letter, a digit, `.`, `_` and `-`), and with XCB_WRAPPED tests name xcb behind the wrapper; the guard has to pass both.
cmake_minimum_required(VERSION 3.24)
project(app_guard_fixture CXX)
enable_testing()

# Stands in for the app library: the guard reads only the name a target links.
add_library(blockwright_app INTERFACE)
set(BW_APP_TEST_ENV ${CMAKE_COMMAND} -E env HOME=${CMAKE_CURRENT_BINARY_DIR}/home)
set(BW_APP_XCB_TEST_ENV bash xcb_test.sh ${CMAKE_CURRENT_BINARY_DIR}/xcb-profile)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../app_guard.cmake)

add_executable(guarded main.cpp)
target_link_libraries(guarded PRIVATE blockwright_app)
set_target_properties(guarded PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_ENV}")
add_executable(not-app main.cpp)
add_executable(xcb main.cpp)
target_link_libraries(xcb PRIVATE blockwright_app)
add_test(NAME xcb COMMAND ${BW_APP_XCB_TEST_ENV} $<TARGET_FILE:xcb>)

set(BW_APP_TEST_BINARIES guarded)
bw_add_app_guard_test()

if(LATE)
  add_executable(late main.cpp)
  target_link_libraries(late PRIVATE blockwright_app)
  add_subdirectory(sub)
  add_executable(late-listed main.cpp)
  target_link_libraries(late-listed PRIVATE blockwright_app)
  set_target_properties(late-listed PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_ENV}")
  list(APPEND BW_APP_TEST_BINARIES late-listed)
  add_executable(blockwright-app-xcb-tests main.cpp)
  target_link_libraries(blockwright-app-xcb-tests PRIVATE blockwright_app)
endif()

if(XCB_BAD)
  foreach(t xcb-target-file xcb-emulator xcb-also-bare)
    add_executable(${t} main.cpp)
    target_link_libraries(${t} PRIVATE blockwright_app)
  endforeach()
  set_target_properties(xcb-target-file xcb-emulator PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_XCB_TEST_ENV}")
  add_test(NAME xcb-target-file COMMAND $<TARGET_FILE:xcb-target-file>)
  add_test(NAME xcb-emulator COMMAND xcb-emulator)
  add_test(NAME xcb-also-bare COMMAND ${BW_APP_XCB_TEST_ENV} $<TARGET_FILE:xcb-also-bare>)
  add_test(NAME xcb-also-bare-unwrapped COMMAND $<TARGET_FILE:xcb-also-bare>)
endif()

if(XCB_SHELL)
  add_test(NAME xcb-shell-unwrapped COMMAND bash -c "exec \"$<TARGET_FILE:xcb>\"")
endif()

if(XCB_BARE)
  add_test(NAME xcb-bare COMMAND $<TARGET_FILE:xcb>)
endif()

if(XCB_RELATIVE)
  add_test(NAME xcb-relative
    COMMAND bash -c "exec './$<TARGET_FILE_NAME:xcb>'"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:xcb>")
  add_executable(not-xcb main.cpp)
  add_test(NAME not-xcb
    COMMAND bash -c "exec './$<TARGET_FILE_NAME:not-xcb>'"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:not-xcb>")
endif()

if(XCB_MENTION)
  add_test(NAME mention-argument
    COMMAND ${CMAKE_COMMAND} -E echo "backend=xcb")
  add_test(NAME mention-quoted
    COMMAND ${CMAKE_COMMAND} -E echo "backend='xcb'")
  add_test(NAME mention-path
    COMMAND ${CMAKE_COMMAND} -E echo "/build/xcb/data")
  add_test(NAME harmless-quoted
    COMMAND bash -c "backend='xcb' printf '%s\\n' ok")
endif()

if(XCB_SUBSTRING)
  add_test(NAME substring-path
    COMMAND ${CMAKE_COMMAND} -E echo "/build/xcb-data/")
  add_test(NAME substring-names
    COMMAND ${CMAKE_COMMAND} -E echo "libxcb.so" "--exe=xcb_test.sh" "backend=not-xcb" "xcb.conf")
  add_test(NAME substring-left
    COMMAND ${CMAKE_COMMAND} -E echo "Axcb" "axcb" "2xcb" ".xcb" "_xcb" "-xcb")
  add_test(NAME substring-right
    COMMAND ${CMAKE_COMMAND} -E echo "xcbA" "xcba" "xcb2" "xcb." "xcb_" "xcb-")
endif()

if(XCB_WRAPPED)
  add_test(NAME wrapped-relative
    COMMAND ${BW_APP_XCB_TEST_ENV} bash -c "exec './$<TARGET_FILE_NAME:xcb>'"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:xcb>")
  add_test(NAME wrapped-exe-prefix
    COMMAND ${BW_APP_XCB_TEST_ENV} bash -c "exec \"./\${1#--exe=}\"" _ --exe=xcb
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:xcb>")
  add_test(NAME wrapped-mention
    COMMAND ${BW_APP_XCB_TEST_ENV} ${CMAKE_COMMAND} -E echo "backend='xcb'")
endif()

if(XCB_OPTION)
  add_test(NAME xcb-option
    COMMAND bash -c "exec \"\${1#--exe=}\"" _ "--exe=$<TARGET_FILE:xcb>")
  add_test(NAME xcb-exe-prefix
    COMMAND bash -c "exec \"./\${1#--exe=}\"" _ --exe=xcb
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:xcb>")
  add_test(NAME xcb-bare-option
    COMMAND bash -c "PATH=. exec \"\${1#--exe=}\"" _ "--exe=xcb"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:xcb>")
  add_test(NAME xcb-word
    COMMAND bash -c "PATH=. exec xcb"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:xcb>")
endif()
