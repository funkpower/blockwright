# bw_add_app_guard_test() adds app_tests_all_guarded: every test binary built on
# the app (linked to blockwright_app) in this directory or one below it is in
# BW_APP_TEST_BINARIES, so app_tests_leave_profile_alone runs it, and runs under
# CROSSCOMPILING_EMULATOR ${BW_APP_TEST_RUNNER}, the scratch profile. A binary
# that needs an X server can't run offscreen; its tests run it through
# ${BW_APP_XCB_TEST_ENV} instead (xcb_test.sh), which runs it under an empty
# profile of its own and fails the run that leaves anything in it, so that
# counts for both. Whether they do is read from the commands ctest runs, not
# from the target: CMake can drop an emulator from a test's command (under
# CMP0158, or for a $<TARGET_FILE> command), so app_guard_check.cmake is given
# where each other binary is built and looks for it in every test's command
# (see there). Those paths reach it in a file written at generate time, one per
# configuration, and not as $<TARGET_FILE> in its command: from CMake 4.5 every
# target a test command names in a generator expression is a build dependency
# of the test, and ctest won't run a test whose dependency isn't built (Not
# Run), so in an unbuilt tree the guard wouldn't run to fail it. It looks at
# the targets once the directory has been read to the end, so one declared
# after the call is seen, against BW_APP_TEST_BINARIES as it stands at the call.
function(bw_add_app_guard_test)
  cmake_language(EVAL CODE "cmake_language(DEFER CALL _bw_add_app_guard_test [[${BW_APP_TEST_BINARIES}]])")
endfunction()

function(_bw_add_app_guard_test guarded)
  set(bw_app_linked)
  set(bw_app_profiled)
  set(bw_app_xcb)
  set(bw_app_xcb_files)
  _bw_targets_below(${CMAKE_CURRENT_SOURCE_DIR} bw_test_targets)
  foreach(t IN LISTS bw_test_targets)
    get_target_property(libs ${t} LINK_LIBRARIES)
    if(libs AND "blockwright_app" IN_LIST libs)
      list(APPEND bw_app_linked ${t})
      get_target_property(emulator ${t} CROSSCOMPILING_EMULATOR)
      if(emulator STREQUAL "${BW_APP_TEST_RUNNER}")
        list(APPEND bw_app_profiled ${t})
      else()
        list(APPEND bw_app_xcb ${t})
        string(APPEND bw_app_xcb_files "set(FILE_${t} [==[$<TARGET_FILE:${t}>]==])\n")
      endif()
    endif()
  endforeach()
  string(JOIN , bw_app_linked_arg ${bw_app_linked})
  string(JOIN , bw_app_guarded_arg ${guarded})
  string(JOIN , bw_app_profiled_arg ${bw_app_profiled})
  string(JOIN , bw_app_xcb_arg ${bw_app_xcb})
  string(JOIN | bw_app_xcb_env_arg ${BW_APP_XCB_TEST_ENV})
  set(bw_app_xcb_files_file ${CMAKE_CURRENT_BINARY_DIR}/app_guard_files-$<CONFIG>.cmake)
  file(GENERATE OUTPUT ${bw_app_xcb_files_file} CONTENT "${bw_app_xcb_files}")
  add_test(NAME app_tests_all_guarded
    COMMAND ${CMAKE_COMMAND} -DLINKED=${bw_app_linked_arg} -DGUARDED=${bw_app_guarded_arg}
            -DPROFILED=${bw_app_profiled_arg} -DXCB=${bw_app_xcb_arg} -DFILES=${bw_app_xcb_files_file}
            -DXCB_ENV=${bw_app_xcb_env_arg} -DCTEST=${CMAKE_CTEST_COMMAND} -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DCONFIG=$<CONFIG> -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/app_guard_check.cmake)
endfunction()

function(_bw_targets_below dir out)
  get_directory_property(targets DIRECTORY ${dir} BUILDSYSTEM_TARGETS)
  get_directory_property(subdirs DIRECTORY ${dir} SUBDIRECTORIES)
  foreach(sub IN LISTS subdirs)
    _bw_targets_below(${sub} below)
    list(APPEND targets ${below})
  endforeach()
  set(${out} ${targets} PARENT_SCOPE)
endfunction()
