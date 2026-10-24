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
  test_cli_shade.cpp
  test_composite.cpp
  test_cp437.cpp
  test_detail.cpp
  test_draw.cpp
  test_edit.cpp
  test_form_brush.cpp
  test_golden.cpp
  test_gradient.cpp
  test_irc.cpp
  test_irc_fit.cpp
  test_layers.cpp
  test_library.cpp
  test_library_bundled.cpp
  test_lint.cpp
  test_lint_rules.cpp
  test_irc_view.cpp
  test_mirc.cpp
  test_outline.cpp
  test_outline_property.cpp
  test_pipeline.cpp
  test_project.cpp
  test_shading_brushes.cpp
  test_smudge_brush.cpp
  test_soft_brush.cpp
  test_sauce.cpp
  test_selection.cpp
  util.h
)
# libzip and zstd to write the broken files test_project.cpp reads
target_link_libraries(blockwright-tests PRIVATE blockwright::core blockwright_cli_io libzip::zip ${BW_ZSTD}
                                                Qt6::Core Catch2::Catch2WithMain)
target_compile_definitions(blockwright-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                    BW_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/catch-tmp"
                                                    BW_BUNDLED_LIBRARY="${PROJECT_SOURCE_DIR}/library")
catch_discover_tests(blockwright-tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh
          $<TARGET_FILE:blockwright-cli> ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-tmp)
set_tests_properties(cli PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
add_test(NAME casefold
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/casefold_test.sh
          $<TARGET_FILE:blockwright-cli> ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/casefold-tmp)
set_tests_properties(casefold PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen SKIP_RETURN_CODE 77)

# The bundled Library passes its own check, and installs where the installed
# CLI finds it.
add_test(NAME library_bundled_check
  COMMAND $<TARGET_FILE:blockwright-cli> library check ${PROJECT_SOURCE_DIR}/library)
set_tests_properties(library_bundled_check PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
add_test(NAME library_install
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/library_install_test.sh
          ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/library-install-tmp
          ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DATADIR} "$<CONFIG>"
          ${PROJECT_SOURCE_DIR}/library)
set_tests_properties(library_install PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

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
  test_irc_render.cpp
  util.h
)
target_link_libraries(blockwright-render-tests PRIVATE blockwright::render Catch2::Catch2WithMain)
target_compile_definitions(blockwright-render-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
catch_discover_tests(blockwright-render-tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME cli_render
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_render_test.sh
          $<TARGET_FILE:blockwright-cli> ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-render-tmp)
set_tests_properties(cli_render PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# The app tests run with HOME and the XDG directories in the build tree, so on
# Linux nothing a test writes past its scratch directory reaches the user's
# real profile (macOS and Windows keep native settings outside HOME). Catch's
# PROPERTIES holds one environment variable and splits a list, so the profile
# goes in through the command Catch runs each test (and its discovery) with.
set(BW_APP_TEST_HOME ${CMAKE_CURRENT_BINARY_DIR}/app-test-home)
file(MAKE_DIRECTORY ${BW_APP_TEST_HOME})
set(BW_APP_TEST_ENV ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
  HOME=${BW_APP_TEST_HOME}/home XDG_CONFIG_HOME=${BW_APP_TEST_HOME}/config
  XDG_DATA_HOME=${BW_APP_TEST_HOME}/data XDG_CACHE_HOME=${BW_APP_TEST_HOME}/cache
  XDG_STATE_HOME=${BW_APP_TEST_HOME}/state)

add_executable(blockwright-app-tests
  test_app.cpp
  test_app_brush.cpp
  test_app_layers.cpp
  test_app_cli_shade.cpp
  test_app_draw.cpp
  test_app_format.cpp
  test_app_irc.cpp
  test_app_library.cpp
  test_app_lint.cpp
  test_app_select.cpp
  test_app_shading.cpp
  test_app_gradient.cpp
  test_app_perf.cpp
  test_app_detail.cpp
  test_app_workspace.cpp
  test_app_project.cpp
  scratch_settings.h
  system_input.h
  util.h
)
# QWindowSystemInterface, to deliver input as the platform does
if(NOT TARGET Qt6::GuiPrivate)
  set(QT_NO_PRIVATE_MODULE_WARNING ON)
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()
target_link_libraries(blockwright-app-tests PRIVATE blockwright_app blockwright_cli_io Qt6::GuiPrivate Catch2::Catch2)
target_compile_definitions(blockwright-app-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                        BW_BUNDLED_LIBRARY="${PROJECT_SOURCE_DIR}/library")
set_target_properties(blockwright-app-tests PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_ENV}")
catch_discover_tests(blockwright-app-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)

# File > Publish to Library and Send to WeeChat: its main points the settings
# and the Library at scratch folders before the QApplication exists.
add_executable(blockwright-publish-tests
  test_app_publish.cpp
  scratch_settings.h
  util.h
)
target_link_libraries(blockwright-publish-tests PRIVATE blockwright_app Catch2::Catch2)
target_compile_definitions(blockwright-publish-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
catch_discover_tests(blockwright-publish-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)

# On an X server of their own (xcb_test.sh), with input through XTest:
# skipped where Xvfb or the xcb libraries aren't installed.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(XCB_TEST IMPORTED_TARGET xcb xcb-xtest)
endif()
if(XCB_TEST_FOUND)
  add_executable(blockwright-app-xcb-tests test_app_xcb.cpp)
  target_link_libraries(blockwright-app-xcb-tests PRIVATE blockwright_app Catch2::Catch2 PkgConfig::XCB_TEST)
  add_test(NAME app_xcb
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/xcb_test.sh $<TARGET_FILE:blockwright-app-xcb-tests>)
  set_tests_properties(app_xcb PROPERTIES SKIP_RETURN_CODE 77)
endif()

# At a fractional device pixel ratio: its main sets QT_SCALE_FACTOR before the
# QApplication exists.
add_executable(blockwright-app-scale-tests
  test_app_scale.cpp
  scratch_settings.h
)
target_link_libraries(blockwright-app-scale-tests PRIVATE blockwright_app Catch2::Catch2)
set_target_properties(blockwright-app-scale-tests PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_ENV}")
catch_discover_tests(blockwright-app-scale-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)

# Every QSettings in the sources comes from the settings factory, so the
# scratch settings the app tests inject cover all of them. The guard reads
# tokens, not lines, and its fixtures hold it to each way of building one.
add_executable(bw-settings-guard settings_guard.cpp)
add_test(NAME app_settings_factory
  COMMAND bw-settings-guard --allow ${PROJECT_SOURCE_DIR}/app/settings.cpp
          ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/render ${PROJECT_SOURCE_DIR}/cli)
add_test(NAME app_settings_guard_fixtures
  COMMAND bw-settings-guard --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/data/settings-guard)

# Run whole under a fresh HOME and XDG profile, the app test binaries leave it
# empty: Blockwright's settings go to the factory's scratch INI file and Qt's
# own (the QtProject store QFileDialog keeps its history in) to the scratch
# directory as well.
add_test(NAME app_tests_leave_profile_alone
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_profile_test.sh ${CMAKE_CURRENT_BINARY_DIR}/app-profile-check
          $<TARGET_FILE:blockwright-app-tests> $<TARGET_FILE:blockwright-app-scale-tests>)

add_test(NAME gui_version
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/gui_version_test.sh $<TARGET_FILE:blockwright>)
