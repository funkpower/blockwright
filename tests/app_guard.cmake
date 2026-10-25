# bw_add_app_guard_test() adds app_tests_all_guarded: every test binary built on
# the app (linked to blockwright_app) in this directory or one below it is in
# BW_APP_TEST_BINARIES, so app_tests_leave_profile_alone runs it, and runs under
# CROSSCOMPILING_EMULATOR ${BW_APP_TEST_ENV}, the scratch profile. The xcb
# binary is the one exception: xcb_test.sh runs it on an X server of its own,
# which the offscreen profile check can't. It looks at the targets once the
# directory has been read to the end, so one declared after the call is seen,
# against BW_APP_TEST_BINARIES as it stands at the call.
function(bw_add_app_guard_test)
  cmake_language(EVAL CODE "cmake_language(DEFER CALL _bw_add_app_guard_test [[${BW_APP_TEST_BINARIES}]])")
endfunction()

function(_bw_add_app_guard_test guarded)
  set(bw_app_linked)
  set(bw_app_profiled)
  _bw_targets_below(${CMAKE_CURRENT_SOURCE_DIR} bw_test_targets)
  foreach(t IN LISTS bw_test_targets)
    get_target_property(libs ${t} LINK_LIBRARIES)
    if(libs AND "blockwright_app" IN_LIST libs AND NOT t STREQUAL "blockwright-app-xcb-tests")
      list(APPEND bw_app_linked ${t})
      get_target_property(emulator ${t} CROSSCOMPILING_EMULATOR)
      if(emulator STREQUAL "${BW_APP_TEST_ENV}")
        list(APPEND bw_app_profiled ${t})
      endif()
    endif()
  endforeach()
  string(JOIN , bw_app_linked_arg ${bw_app_linked})
  string(JOIN , bw_app_guarded_arg ${guarded})
  string(JOIN , bw_app_profiled_arg ${bw_app_profiled})
  add_test(NAME app_tests_all_guarded
    COMMAND ${CMAKE_COMMAND} -DLINKED=${bw_app_linked_arg} -DGUARDED=${bw_app_guarded_arg}
            -DPROFILED=${bw_app_profiled_arg} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/app_guard_check.cmake)
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
