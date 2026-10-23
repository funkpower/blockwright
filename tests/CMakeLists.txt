find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  include(FetchContent)
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
include(Catch)

add_executable(blockwright-tests
  test_ansi_parse.cpp
  test_ansi_write.cpp
  test_brush.cpp
  test_cli_read.cpp
  test_cp437.cpp
  test_detail.cpp
  test_draw.cpp
  test_edit.cpp
  test_golden.cpp
  test_irc.cpp
  test_lint.cpp
  test_mirc.cpp
  test_sauce.cpp
  test_selection.cpp
  util.h
)
target_link_libraries(blockwright-tests PRIVATE blockwright::core blockwright_cli_io Catch2::Catch2WithMain)
target_compile_definitions(blockwright-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                    BW_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/catch-tmp")
catch_discover_tests(blockwright-tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh
          $<TARGET_FILE:blockwright-cli> ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-tmp)
set_tests_properties(cli PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME irc_oracle
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/irc_oracle_test.sh
          $<TARGET_FILE:blockwright-cli> ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/irc-oracle-tmp)
set_tests_properties(irc_oracle PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME install_dev_paths
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_dev_test.sh
          ${PROJECT_SOURCE_DIR}/scripts/install-dev ${CMAKE_CURRENT_BINARY_DIR}/install-dev-tmp)

add_test(NAME install_notices
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_test.sh
          ${PROJECT_BINARY_DIR} ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-tmp
          ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DOCDIR} "$<CONFIG>")
add_test(NAME install_notices_absolute_docdir
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
          ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-outside-tmp
          ${CMAKE_GENERATOR} ${Qt6_DIR})
# The same with a space in every scratch path, so an unquoted path in either
# script splits and the install escapes the staging dir
add_test(NAME install_notices_space_path
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
          ${PROJECT_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/install notices space-tmp"
          ${CMAKE_GENERATOR} ${Qt6_DIR})
set_tests_properties(install_notices install_notices_absolute_docdir install_notices_space_path
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# And under a multi-config generator, where CMAKE_BUILD_TYPE is ignored and
# build and install must name the same configuration
if(CMAKE_GENERATOR MATCHES "^Ninja")
  add_test(NAME install_notices_multi_config
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
            ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-multi-tmp
            "Ninja Multi-Config" ${Qt6_DIR})
  set_tests_properties(install_notices_multi_config PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endif()

add_test(NAME forge_merge
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/forge_merge_test.sh
          ${PROJECT_SOURCE_DIR}/scripts/forge ${CMAKE_CURRENT_BINARY_DIR}/forge-merge-tmp)
set_tests_properties(forge_merge PROPERTIES SKIP_RETURN_CODE 77)

add_executable(blockwright-render-tests
  test_canvas_renderer.cpp
  test_render.cpp
  test_render_golden.cpp
  util.h
)
target_link_libraries(blockwright-render-tests PRIVATE blockwright::render Catch2::Catch2WithMain)
target_compile_definitions(blockwright-render-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
catch_discover_tests(blockwright-render-tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME cli_render
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_render_test.sh
          $<TARGET_FILE:blockwright-cli> ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-render-tmp)
set_tests_properties(cli_render PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_executable(blockwright-app-tests
  test_app.cpp
  test_app_brush.cpp
  test_app_draw.cpp
  test_app_format.cpp
  test_app_select.cpp
  test_app_perf.cpp
  test_app_detail.cpp
  system_input.h
  util.h
)
# QWindowSystemInterface, to deliver input as the platform does
if(NOT TARGET Qt6::GuiPrivate)
  set(QT_NO_PRIVATE_MODULE_WARNING ON)
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()
target_link_libraries(blockwright-app-tests PRIVATE blockwright_app Qt6::GuiPrivate Catch2::Catch2)
target_compile_definitions(blockwright-app-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
catch_discover_tests(blockwright-app-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)

# At a fractional device pixel ratio: its main sets QT_SCALE_FACTOR before the
# QApplication exists.
add_executable(blockwright-app-scale-tests
  test_app_scale.cpp
)
target_link_libraries(blockwright-app-scale-tests PRIVATE blockwright_app Catch2::Catch2)
catch_discover_tests(blockwright-app-scale-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)

add_test(NAME gui_version
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/gui_version_test.sh $<TARGET_FILE:blockwright>)
