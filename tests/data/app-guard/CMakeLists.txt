# A project laid out like tests/: app_guard_fixture_test.sh configures it and
# runs its app_tests_all_guarded. With LATE on, an app test binary is declared
# after bw_add_app_guard_test(), another in a subdirectory, and a third is put
# in BW_APP_TEST_BINARIES only after the call, too late for the profile check
# to have run it; the guard has to reject all three.
cmake_minimum_required(VERSION 3.24)
project(app_guard_fixture CXX)
enable_testing()

# Stands in for the app library: the guard reads only the name a target links.
add_library(blockwright_app INTERFACE)
set(BW_APP_TEST_ENV ${CMAKE_COMMAND} -E env HOME=${CMAKE_CURRENT_BINARY_DIR}/home)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../app_guard.cmake)

add_executable(guarded main.cpp)
target_link_libraries(guarded PRIVATE blockwright_app)
set_target_properties(guarded PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_ENV}")
add_executable(not-app main.cpp)

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
endif()
