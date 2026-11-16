# ansidraw.py's directory, which the scene import tests' scenes import
# (tests/scene_env.h); they skip without it. Empty unless set here or in
# the environment at the first configure.
set(BW_ANSIDRAW_DIR "$ENV{BW_ANSIDRAW_DIR}" CACHE PATH
    "Directory holding ansidraw.py, for the scene import tests")

find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  if(NOT BW_FETCH_CATCH2)
    message(FATAL_ERROR
      "The tests need Catch2 3, which wasn't found, and BW_FETCH_CATCH2 is OFF, "
      "so it won't be downloaded. Install Catch2 3 (or name its prefix in "
      "CMAKE_PREFIX_PATH), turn BW_FETCH_CATCH2 on, or configure with "
      "-DBUILD_TESTING=OFF to build without the tests.")
  endif()
  include(FetchContent)
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
include(Catch)

# The shell-script tests run the CLI as $BW_CLI_COMMAND: the program, or in a
# cross build a script running it under the emulator, with the CR of each
# CRLF its text-mode stdout and stderr write taken out.
set(BW_CLI_COMMAND $<TARGET_FILE:blockwright-cli>)
set(BW_GUI_COMMAND $<TARGET_FILE:blockwright>)
if(CMAKE_CROSSCOMPILING)
  set(bw_run_template [[#!/usr/bin/env bash
set -o pipefail
{ "$<TARGET_PROPERTY:PROGRAM,CROSSCOMPILING_EMULATOR>" "$<TARGET_FILE:PROGRAM>" "$@" 2>&1 1>&3 3>&- | tr -d '\r' >&2; } 3>&1 | tr -d '\r'
]])
  foreach(program blockwright-cli blockwright)
    string(REPLACE PROGRAM ${program} bw_run "${bw_run_template}")
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/run-${program} CONTENT "${bw_run}"
      FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  endforeach()
  set(BW_CLI_COMMAND ${CMAKE_CURRENT_BINARY_DIR}/run-blockwright-cli)
  set(BW_GUI_COMMAND ${CMAKE_CURRENT_BINARY_DIR}/run-blockwright)
endif()
# The scripts skip what a Windows CLI can't do: run sh or python3 generators,
# or be stopped by a file size limit; and take its error texts.
set(BW_CLI_ENV QT_QPA_PLATFORM=offscreen)
if(WIN32)
  list(APPEND BW_CLI_ENV BW_CLI_WINDOWS=1)
endif()

add_executable(blockwright-tests
  test_ansi_parse.cpp
  test_ansi_write.cpp
  test_brush.cpp
  test_clone_brush.cpp
  test_cli_read.cpp
  test_regular_file.cpp
  test_cli_replay.cpp
  test_cli_shade.cpp
  test_composite.cpp
  test_cp437.cpp
  test_utf8_ansi.cpp
  test_detail.cpp
  test_texture.cpp
  test_spray.cpp
  test_draw.cpp
  test_edit.cpp
  test_face.cpp
  test_frame.cpp
  test_frame_library.cpp
  test_tdf.cpp
  test_font_store.cpp
  test_form_brush.cpp
  test_golden.cpp
  test_generator.cpp
  test_glyph.cpp
  test_glyph_writers.cpp
  test_gradient.cpp
  test_irc.cpp
  test_irc_attrs.cpp
  test_irc_fit.cpp
  test_irc_target.cpp
  test_layers.cpp
  test_canvas_size.cpp
  test_library.cpp
  test_library_bundled.cpp
  test_stamp_slice.cpp
  test_samples.cpp
  test_lint.cpp
  test_lint_rules.cpp
  test_irc_view.cpp
  test_micron.cpp
  test_micron_target.cpp
  test_mirc.cpp
  test_mirc_sent.cpp
  test_outline.cpp
  test_outline_property.cpp
  test_portable_random.cpp
  test_project.cpp
  test_reference_layers.cpp
  test_shading_brushes.cpp
  test_steps.cpp
  cpu_time.cpp
  cpu_time.h
  test_cpu_time.cpp
  test_smudge_brush.cpp
  test_soft_brush.cpp
  test_sauce.cpp
  test_scene_import.cpp
  scene_env.h
  test_xbin.cpp
  test_width.cpp
  test_selection.cpp
  test_symmetry_stroke.cpp
  test_big_text.cpp
  test_text.cpp
  test_text_presets.cpp
  test_text_style.cpp
  test_wide_pairs.cpp
  test_wide_cells.cpp
  test_wide_irc.cpp
  test_wide_writers.cpp
  test_wide_read.cpp
  util.h
)
if(WIN32)
  # Running generators without sh: bw-gen-helper stands in for one, beside
  # the tests, needing no DLLs in a generator's copy of its files.
  target_sources(blockwright-tests PRIVATE test_generator_win.cpp test_junction_win.cpp)
  add_executable(bw-gen-helper gen_helper_win.cpp)
  target_link_options(bw-gen-helper PRIVATE -static)
  add_dependencies(blockwright-tests bw-gen-helper)
endif()
# The tests' scratch folder. Wine takes a path that starts with / for a host
# path and reads a symbolic link named that way as what it points at (even
# DeleteFileW removes the target), so under wine it is named on wine's drive
# for /, Z:, where links behave as they do on Windows.
set(BW_TEST_TMP "${CMAKE_CURRENT_BINARY_DIR}/catch-tmp")
if(WIN32 AND CMAKE_CROSSCOMPILING)
  set(BW_TEST_TMP "Z:${BW_TEST_TMP}")
endif()
# libzip and zstd to write the broken files test_project.cpp reads
target_link_libraries(blockwright-tests PRIVATE blockwright::core blockwright_cli_io libzip::zip ${BW_ZSTD}
                                                Qt6::Core Catch2::Catch2WithMain)
target_compile_definitions(blockwright-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                    BW_ANSIDRAW_DIR="${BW_ANSIDRAW_DIR}"
                                                    BW_TEST_TMP="${BW_TEST_TMP}"
                                                    BW_BUNDLED_LIBRARY="${PROJECT_SOURCE_DIR}/library"
                                                    BW_SAMPLES_SOURCE="${PROJECT_SOURCE_DIR}/samples"
                                                    BW_BRANDING_DIR="${PROJECT_SOURCE_DIR}/branding"
                                                    BW_SAMPLES_FROM_BINDIR="${BW_SAMPLES_FROM_BINDIR}")
catch_discover_tests(blockwright-tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# hidden ([.pool-full]): it fills the process's cluster pool, so it runs alone
add_test(NAME wide_cells_pool_full
  COMMAND blockwright-tests "a project's clusters open whatever the session's pool holds")
set_tests_properties(wide_cells_pool_full PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-tmp)
set_tests_properties(cli PROPERTIES ENVIRONMENT "${BW_CLI_ENV}")
set_property(TEST cli APPEND PROPERTY ENVIRONMENT "BW_LOGO=${PROJECT_SOURCE_DIR}/branding/logo.txt")
if(CMAKE_CROSSCOMPILING)
  set_tests_properties(cli PROPERTIES TIMEOUT 900)  # some hundred programs under an emulator
endif()
# replay's open-file refusal when setrlimit fails, under an LD_PRELOAD shim
# (macOS has no LD_PRELOAD; an emulated CLI wouldn't load a host library)
if(UNIX AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING)
  add_library(setrlimit-fails MODULE setrlimit_fails.cpp)
  target_link_libraries(setrlimit-fails PRIVATE ${CMAKE_DL_LIBS})
  # uninstrumented in a sanitizer build: it is preloaded into plain bash too,
  # where clang's unresolved ASan runtime references can't load
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-fno-sanitize=all BW_HAVE_FNO_SANITIZE_ALL)
  if(BW_HAVE_FNO_SANITIZE_ALL)
    target_compile_options(setrlimit-fails PRIVATE -fno-sanitize=all)
    target_link_options(setrlimit-fails PRIVATE -fno-sanitize=all)
  endif()
  add_test(NAME cli_rlimit
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_rlimit_test.sh
            ${BW_CLI_COMMAND} $<TARGET_FILE:setrlimit-fails> ${CMAKE_CURRENT_BINARY_DIR}/cli-rlimit-tmp)
  set_tests_properties(cli_rlimit PROPERTIES ENVIRONMENT "${BW_CLI_ENV}" SKIP_RETURN_CODE 77)
  # the same under German messages, where bash's EPERM reads otherwise
  add_test(NAME cli_rlimit_locale
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_rlimit_locale_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/cli_rlimit_test.sh
            ${BW_CLI_COMMAND} $<TARGET_FILE:setrlimit-fails> ${CMAKE_CURRENT_BINARY_DIR}/cli-rlimit-locale-tmp)
  set_tests_properties(cli_rlimit_locale PROPERTIES ENVIRONMENT "${BW_CLI_ENV}" SKIP_RETURN_CODE 77)
endif()
add_test(NAME cli_generators
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_generators_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-generators-tmp)
set_tests_properties(cli_generators PROPERTIES ENVIRONMENT "${BW_CLI_ENV}" SKIP_RETURN_CODE 77)
add_test(NAME cli_scene
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_scene_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-scene-tmp
          "${BW_ANSIDRAW_DIR}")
set_tests_properties(cli_scene PROPERTIES ENVIRONMENT "${BW_CLI_ENV}" SKIP_RETURN_CODE 77)
add_test(NAME cli_scene_fixed
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_scene_fixed_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_BINARY_DIR}/cli-scene-fixed-tmp)
set_tests_properties(cli_scene_fixed PROPERTIES ENVIRONMENT "${BW_CLI_ENV}" SKIP_RETURN_CODE 77)
add_test(NAME casefold
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/casefold_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/casefold-tmp)
set_tests_properties(casefold PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen SKIP_RETURN_CODE 77)

# The bundled Library passes its own check, and installs where the installed
# CLI finds it.
add_test(NAME library_bundled_check
  COMMAND ${BW_CLI_COMMAND} library check ${PROJECT_SOURCE_DIR}/library)
set_tests_properties(library_bundled_check PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
if(NOT CMAKE_CROSSCOMPILING)  # builds and installs on the host
add_test(NAME library_install
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/library_install_test.sh
          ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/library-install-tmp
          ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DATADIR} "$<CONFIG>"
          ${PROJECT_SOURCE_DIR}/library)
set_tests_properties(library_install PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# The sample projects install as they are in the source (samples.h).
add_test(NAME samples_install
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/samples_install_test.sh
          ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/samples-install-tmp
          ${CMAKE_INSTALL_FULL_DATADIR} ${CMAKE_INSTALL_FULL_BINDIR} ${PROJECT_SOURCE_DIR}/samples "$<CONFIG>")
set_tests_properties(samples_install PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endif()

add_test(NAME irc_oracle
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/irc_oracle_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/irc-oracle-tmp)
set_tests_properties(irc_oracle PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
add_test(NAME micron_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/micron_cli_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/micron-cli-tmp)
set_tests_properties(micron_cli PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# NomadNet's own parser reads the golden Micron pages; skipped without nomadnet
set(BW_NOMADNET_PYTHON python3 CACHE STRING "Python that can import nomadnet, for the micron_nomadnet test")
add_test(NAME micron_nomadnet
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/micron_nomadnet_test.sh ${BW_NOMADNET_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(micron_nomadnet PROPERTIES SKIP_RETURN_CODE 77)

# wine-run starts the wine session so its services hold no test's output (#259)
if(CMAKE_CROSSCOMPILING_EMULATOR STREQUAL "${PROJECT_BINARY_DIR}/wine-run")
  add_test(NAME wine_run_session
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/wine_run_test.sh
            ${CMAKE_CROSSCOMPILING_EMULATOR} ${BW_WINESERVER} ${CMAKE_CURRENT_BINARY_DIR}/wine-run-tmp)
  set_tests_properties(wine_run_session PROPERTIES TIMEOUT 900)
endif()

if(NOT CMAKE_CROSSCOMPILING)  # host tools, and builds on the host
add_test(NAME install_dev_paths
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_dev_test.sh
          ${PROJECT_SOURCE_DIR}/scripts/install-dev ${CMAKE_CURRENT_BINARY_DIR}/install-dev-tmp)

add_test(NAME install_notices
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_test.sh
          ${PROJECT_BINARY_DIR} ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-tmp
          ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DOCDIR} "$<CONFIG>")
# the nested builds look where this one does, its list joined with ':'
string(REPLACE ";" ":" bw_prefix_path "${CMAKE_PREFIX_PATH}")
add_test(NAME install_notices_absolute_docdir
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
          ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-outside-tmp
          ${CMAKE_GENERATOR} ${Qt6_DIR} "${bw_prefix_path}")
# The same with a space in every scratch path, so an unquoted path in either
# script splits and the install escapes the staging dir
add_test(NAME install_notices_space_path
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
          ${PROJECT_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/install notices space-tmp"
          ${CMAKE_GENERATOR} ${Qt6_DIR} "${bw_prefix_path}")
set_tests_properties(install_notices install_notices_absolute_docdir install_notices_space_path
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# packaging/freebsd/pkg-plist lists the notices the install carries, no more
add_test(NAME install_notices_freebsd_plist
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_freebsd_plist_test.sh
          ${PROJECT_BINARY_DIR} ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-plist-tmp
          ${CMAKE_INSTALL_FULL_DOCDIR} "$<CONFIG>")
# A BLOCKWRIGHT_SYSTEM_UNIFONT build credits the release its .hex files name,
# or none (render/CMakeLists.txt); the nested builds use the bundled copy.
if(BLOCKWRIGHT_SYSTEM_UNIFONT)
  set_property(TEST install_notices APPEND PROPERTY ENVIRONMENT
    "BW_UNIFONT_CREDIT=Includes ${BW_UNIFONT_RELEASE} Roman Czyborra, Paul Hardy et al.,")
endif()
# gen-unifont --release, which names the release in that credit
add_test(NAME unifont_release
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/unifont_release_test.sh ${PROJECT_SOURCE_DIR}/scripts/gen-unifont
          ${CMAKE_CURRENT_BINARY_DIR}/unifont-release-tmp)
# And under a multi-config generator, where CMAKE_BUILD_TYPE is ignored and
# build and install must name the same configuration
if(CMAKE_GENERATOR MATCHES "^Ninja")
  add_test(NAME install_notices_multi_config
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
            ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-multi-tmp
            "Ninja Multi-Config" ${Qt6_DIR} "${bw_prefix_path}")
  # with CMAKE_CONFIGURATION_TYPES in the environment naming only another one
  add_test(NAME install_notices_multi_config_debug_env
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_notices_outside_test.sh
            ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/install-notices-multi-debug-tmp
            "Ninja Multi-Config" ${Qt6_DIR} "${bw_prefix_path}")
  set_tests_properties(install_notices_multi_config PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
  set_tests_properties(install_notices_multi_config_debug_env PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;CMAKE_CONFIGURATION_TYPES=Debug")
endif()

# The Debian and Fedora packages (packaging/debian, packaging/rpm) build the
# version the tree is
add_test(NAME linux_packages
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/linux_packages_test.sh ${PROJECT_SOURCE_DIR})

# Every packaging/*/THIRD-PARTY file links Qt's source under
# download.qt.io/archive/qt/ through the @QT_MINOR@/@QT_VERSION@ template the
# packaging scripts fill in, never official_releases (which drops old minors)
# and never a fixed version that could drift from the bundled Qt
add_test(NAME packaging_qt_links
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/packaging_qt_links_test.sh ${PROJECT_SOURCE_DIR})
# The same check on trees written at configure time, one case each, in two
# tests: packaging_qt_links_<case> wants the case's ok or FAIL line (ctest then
# ignores the exit code) and packaging_qt_links_<case>_exit wants exit 0 for a
# pass and non-zero for a fail (WILL_FAIL), so a check that prints FAIL but
# exits 0 is red.
# A case may give an environment, such as a BASH_ENV file for bash to source
# before the check to make a tool it calls fail, or a locale.
set(bw_qt_links_cases ${CMAKE_CURRENT_BINARY_DIR}/packaging-qt-links)
function(bw_qt_links_tests name tree outcome expect)
  add_test(NAME packaging_qt_links_${name}
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/packaging_qt_links_test.sh ${tree})
  set_tests_properties(packaging_qt_links_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}")
  add_test(NAME packaging_qt_links_${name}_exit
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/packaging_qt_links_test.sh ${tree})
  if(outcome STREQUAL "fail")
    set_tests_properties(packaging_qt_links_${name}_exit PROPERTIES WILL_FAIL TRUE)
  endif()
  if(ARGC GREATER 4)
    set_tests_properties(packaging_qt_links_${name} packaging_qt_links_${name}_exit PROPERTIES
      ENVIRONMENT "${ARGV4}")
  endif()
endfunction()
function(bw_qt_links_case name outcome file content expect)
  # an optional environment follows
  set(tree ${bw_qt_links_cases}/${name})
  file(REMOVE_RECURSE ${tree})
  if(NOT file STREQUAL "")
    file(WRITE ${tree}/packaging/${file} "${content}")
  else()
    file(MAKE_DIRECTORY ${tree}/packaging/linux)
  endif()
  bw_qt_links_tests(${name} ${tree} ${outcome} "${expect}" ${ARGN})
endfunction()
set(bw_qt_tpl "https://download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/")
set(bw_qt_ok "^ok   packaging/linux/THIRD-PARTY.txt links Qt's source under download.qt.io/archive/qt/ \\(1\\)\n$")
bw_qt_links_case(template pass linux/THIRD-PARTY.txt "- Qt @QT_VERSION@\nSource:\n  ${bw_qt_tpl}\n" "${bw_qt_ok}")
bw_qt_links_case(dead fail windows/THIRD-PARTY.txt "Source:\n  https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/\n"
  "FAIL packaging/windows/THIRD-PARTY.txt links under official_releases")
bw_qt_links_case(fixed_link fail windows/THIRD-PARTY.txt "Source:\n  https://download.qt.io/archive/qt/6.8/6.8.3/submodules/\n"
  "FAIL packaging/windows/THIRD-PARTY.txt has a Qt source link other than .* https://download.qt.io/archive/qt/6.8/6.8.3/submodules/")
bw_qt_links_case(fixed_name fail windows/THIRD-PARTY.txt "- Qt 6.9.1 (Core)\nSource:\n  ${bw_qt_tpl}\n"
  "FAIL packaging/windows/THIRD-PARTY.txt names a fixed Qt version instead of @QT_VERSION@: 1:- Qt 6.9.1")
bw_qt_links_case(no_scheme fail macos/THIRD-PARTY.txt "Source:\n  download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/\n"
  "FAIL packaging/macos/THIRD-PARTY.txt has a Qt source link other than .* download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/")
bw_qt_links_case(upper_case fail macos/THIRD-PARTY.txt "Source:\n  HTTPS://DOWNLOAD.QT.IO/OFFICIAL/qt/6.8/6.8.3/submodules/\n"
  "FAIL packaging/macos/THIRD-PARTY.txt has a Qt source link other than .* HTTPS://DOWNLOAD.QT.IO/OFFICIAL/qt/6.8/6.8.3/submodules/")
bw_qt_links_case(upper_dead fail macos/THIRD-PARTY.txt "Source:\n  https://download.qt.io/OFFICIAL_RELEASES/qt/6.8/6.8.3/submodules/\n"
  "FAIL packaging/macos/THIRD-PARTY.txt links under official_releases")
bw_qt_links_case(none fail "" "" "FAIL no THIRD-PARTY file under packaging/")
# A fixed version however it is spaced, worded or cased: two spaces, a tab,
# "Qt version", "Qt6", lower case and a non-breaking space
set(bw_qt_fixed "FAIL packaging/windows/THIRD-PARTY.txt names a fixed Qt version instead of @QT_VERSION@: 1:- ")
string(ASCII 194 160 bw_nbsp)
foreach(c "two_spaces;Qt  6.9.1;Qt 6\\.9\\.1" "tab;Qt\t6.9.1;Qt\t6\\.9\\.1" "version_word;Qt version 6.9.1;Qt version 6\\.9\\.1"
          "qt6;Qt6 6.9.1;Qt6 6\\.9\\.1" "lower;qt 6.9.1;qt 6\\.9\\.1" "nbsp;Qt${bw_nbsp}6.9.1;Qt${bw_nbsp}6\\.9\\.1"
          # a v before the version, with and without a word; a hyphen with no
          # word and a run holding a hyphen after one, which are no licence id;
          # and Qt's source tarball names
          "v_prefix;Qt v6.9.1;Qt v6\\.9\\.1" "version_v;Qt version: v6.9.1;Qt version: v6\\.9\\.1"
          "hyphen;Qt-6.9.1;Qt-6\\.9\\.1" "version_dash;Qt version - 6.9.1;Qt version - 6\\.9\\.1"
          "tarball;qt-everywhere-src-6.9.1.tar.xz;qt-everywhere-src-6\\.9\\.1\\.tar\\.xz"
          "tarball_module;qtbase-everywhere-src-6.9.1.tar.xz;qtbase-everywhere-src-6\\.9\\.1\\.tar\\.xz"
          "tarball_opensource;qt-everywhere-opensource-src-5.15.2.tar.xz;qt-everywhere-opensource-src-5\\.15\\.2\\.tar\\.xz")
  list(GET c 0 name)
  list(GET c 1 text)
  list(GET c 2 want)
  bw_qt_links_case(fixed_name_${name} fail windows/THIRD-PARTY.txt "- ${text} (Core)\nSource:\n  ${bw_qt_tpl}\n"
    "${bw_qt_fixed}${want} \\(Core\\)")
endforeach()
# A lone Latin-1 non-breaking space under a UTF-8 locale, where it is no
# character at all unless the check reads bytes
string(ASCII 160 bw_latin1_nbsp)
bw_qt_links_case(fixed_name_latin1_nbsp fail windows/THIRD-PARTY.txt "- Qt${bw_latin1_nbsp}6.9.1 (Core)\nSource:\n  ${bw_qt_tpl}\n"
  "${bw_qt_fixed}Qt.6\\.9\\.1 \\(Core\\)" LC_ALL=C.UTF-8)
# What the real notices say about Qt and its version is no fixed version
bw_qt_links_case(fixed_name_lookalikes pass linux/THIRD-PARTY.txt
  "- Qt @QT_VERSION@ and Qt6 @QT_VERSION@ (Core)\n  builds of the same Qt version: copy them over\n  Qt6ConfigVersion 6.9 is read\nSource:\n  ${bw_qt_tpl}\n"
  "${bw_qt_ok}")
# A licence id, a word joined to its X.Y by a hyphen, is no Qt version: in
# brackets, after an em dash, and two of them
string(ASCII 226 128 148 bw_em_dash)
foreach(c "licence_brackets;- Qt (LGPL-3.0-only)" "licence_em_dash;- Qt ${bw_em_dash} LGPL-3.0-only"
          "licence_or;Qt (GPL-2.0 or LGPL-3.0)")
  list(GET c 0 name)
  list(GET c 1 text)
  bw_qt_links_case(fixed_name_${name} pass linux/THIRD-PARTY.txt "${text}\nSource:\n  ${bw_qt_tpl}\n" "${bw_qt_ok}")
endforeach()
# A THIRD-PARTY* that is a directory or a symlink, and a symlink to a
# directory, which find does not go into, fail rather than being skipped
set(bw_qt_dead "https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/\n")
bw_qt_links_case(directory fail linux/THIRD-PARTY.txt "Source:\n  ${bw_qt_tpl}\n"
  "FAIL packaging/macos/THIRD-PARTY is not a regular file")
file(WRITE ${bw_qt_links_cases}/directory/packaging/macos/THIRD-PARTY/qt.txt "${bw_qt_dead}")
foreach(c symlink symlink_dir)
  set(tree ${bw_qt_links_cases}/${c})
  file(REMOVE_RECURSE ${tree})
  file(WRITE ${tree}/packaging/linux/THIRD-PARTY.txt "Source:\n  ${bw_qt_tpl}\n")
  file(WRITE ${tree}/elsewhere/THIRD-PARTY.txt "${bw_qt_dead}")
endforeach()
file(MAKE_DIRECTORY ${bw_qt_links_cases}/symlink/packaging/macos)
file(CREATE_LINK ../../elsewhere/THIRD-PARTY.txt ${bw_qt_links_cases}/symlink/packaging/macos/THIRD-PARTY.txt SYMBOLIC)
bw_qt_links_tests(symlink ${bw_qt_links_cases}/symlink fail
  "FAIL packaging/macos/THIRD-PARTY.txt is a symlink, not a regular file")
file(CREATE_LINK ../elsewhere ${bw_qt_links_cases}/symlink_dir/packaging/macos SYMBOLIC)
bw_qt_links_tests(symlink_dir ${bw_qt_links_cases}/symlink_dir fail
  "FAIL packaging/macos is a symlink to a directory, which the check does not go into")
# A NUL byte, which makes grep call the file binary and print no line
set(tree ${bw_qt_links_cases}/nul_byte)
file(REMOVE_RECURSE ${tree})
file(MAKE_DIRECTORY ${tree}/packaging/windows)
execute_process(COMMAND printf "Qt\\0notice\\nSource:\\n  ${bw_qt_dead}"
  OUTPUT_FILE ${tree}/packaging/windows/THIRD-PARTY.txt COMMAND_ERROR_IS_FATAL ANY)
bw_qt_links_tests(nul_byte ${tree} fail "FAIL packaging/windows/THIRD-PARTY.txt links under official_releases")
# An error stops the check with a non-zero exit even where it would
# otherwise pass: grep failing (rc 2, which g() tells from no match) and
# another tool failing (set -e) on the template tree
file(WRITE ${bw_qt_links_cases}/grep-error.bash "grep() { return 2; }\n")
bw_qt_links_tests(grep_error ${bw_qt_links_cases}/template fail
  "FAIL grep .* failed \\(rc 2\\).*FAIL stopped before checking every file \\(rc 1\\)"
  BASH_ENV=${bw_qt_links_cases}/grep-error.bash)
file(WRITE ${bw_qt_links_cases}/tool-error.bash "tr() { return 1; }\n")
bw_qt_links_tests(tool_error ${bw_qt_links_cases}/template fail
  "FAIL stopped before checking every file \\(rc 1\\)"
  BASH_ENV=${bw_qt_links_cases}/tool-error.bash)

# packaging/windows/third-party fills the Windows Qt's version into the
# Windows THIRD-PARTY.txt
add_test(NAME windows_third_party
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/windows_third_party_test.sh ${PROJECT_SOURCE_DIR}
          ${CMAKE_CURRENT_BINARY_DIR}/windows-third-party)

# packaging/qt-third-party writes each value into its placeholder as it is
# and fails, writing nothing, on a leftover placeholder or an @ in a value
add_test(NAME qt_third_party
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/qt_third_party_test.sh ${PROJECT_SOURCE_DIR}
          ${CMAKE_CURRENT_BINARY_DIR}/qt-third-party)

# packaging/macos/third-party puts Qt's notice (its version, frameworks,
# plugins and source link), the LGPL, Qt's SBOMs and the libzip and zstd
# licences into the app's Contents/Resources/doc, on fake Qt prefixes and
# bundles. macos_third_party checks what lands and that a run, failing or
# not, leaves nothing in TMPDIR and a failing one adds nothing to the app;
# macos_third_party_<case>_exit wants the script's own exit code, 0
# for ok and pdf, non-zero (WILL_FAIL) for the rest.
add_test(NAME macos_third_party
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/macos_third_party_test.sh ${PROJECT_SOURCE_DIR}
          ${CMAKE_CURRENT_BINARY_DIR}/macos-third-party)
foreach(case ok pdf no_version no_sbom unknown_framework unknown_plugin no_deps_licence no_deps_stamp no_license
        leftover_placeholder)
  add_test(NAME macos_third_party_${case}_exit
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/macos_third_party_test.sh ${PROJECT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/macos-third-party-exit ${case})
  if(NOT case MATCHES "^(ok|pdf)$")
    set_tests_properties(macos_third_party_${case}_exit PROPERTIES WILL_FAIL TRUE)
  endif()
endforeach()

# packaging/debian/copyright gives each file the licence its header or its
# third_party/ README states (DEP-5, last match wins)
find_program(BW_PYTHON3 NAMES python3)
if(BW_PYTHON3)
  add_test(NAME debian_copyright
    COMMAND ${BW_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/debian_copyright_test.py ${PROJECT_SOURCE_DIR})
endif()

# The samples checked in are what bw-make-samples draws now, byte for byte
# (PLAN §12). The maker runs the samples' generators, one of them in python3.
if(BW_PYTHON3 AND UNIX AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME samples_remade
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/samples_remade_test.sh
            $<TARGET_FILE:bw-make-samples> ${PROJECT_SOURCE_DIR}/samples ${CMAKE_CURRENT_BINARY_DIR}/samples-remade-tmp)
  set_tests_properties(samples_remade PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen TIMEOUT 300)
endif()

# The desktop entry, icons and AppStream metadata of a Linux install, the
# AppImage's AppRun, and the AppImage scripts/package-appimage made, when
# BW_APPIMAGE names it
if(UNIX AND NOT APPLE)
  add_test(NAME appimage_apprun
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/apprun_test.sh ${PROJECT_SOURCE_DIR}/packaging/linux/AppRun
            ${CMAKE_CURRENT_BINARY_DIR}/apprun-tmp)
  if(BW_APPIMAGE)
    add_test(NAME appimage
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/appimage_test.sh ${BW_APPIMAGE}
              ${CMAKE_CURRENT_BINARY_DIR}/appimage-tmp ${CMAKE_CURRENT_SOURCE_DIR}/data)
  endif()
  add_test(NAME linux_desktop
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/linux_desktop_test.sh
            ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/linux-desktop-tmp
            ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DATADIR} "$<CONFIG>")
  # The Flatpak manifest, statically (the build itself is scripts/package-flatpak),
  # and each way of breaking it the lint must catch
  add_test(NAME flatpak_manifest
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/flatpak_manifest_test.sh ${PROJECT_SOURCE_DIR})
  add_test(NAME flatpak_manifest_self_test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/flatpak_manifest_test.sh --self-test ${PROJECT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/flatpak-manifest-tmp)
endif()

# Distribution builds have no network: BW_FETCH_CATCH2=OFF never downloads,
# and BUILD_TESTING=OFF needs no Catch2
if(Catch2_FOUND)
  set(bw_catch2_dir ${Catch2_DIR})
else()
  set(bw_catch2_dir "")
endif()
add_test(NAME catch2_offline
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/catch2_offline_test.sh
          ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/catch2-offline-tmp
          ${CMAKE_GENERATOR} ${Qt6_DIR} "${bw_prefix_path}" "${bw_catch2_dir}")

# A distribution's Catch2 may be older than 3.8, whose catch_discover_tests
# runs every test name after an unmatched [ into one test that runs nothing
# (catch_names_check.cmake): no TEST_CASE name has one, and the check finds one.
add_test(NAME catch_names
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/catch_names_check.cmake)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/catch-names-bad/bad.cpp
  "TEST_CASE(\"Ctrl+[ and Ctrl+] are matched\") {}\nTEST_CASE(\"a [ alone \"\n          \"over two lines\", \"[tag]\") {}\n")
add_test(NAME catch_names_finds_unmatched
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_BINARY_DIR}/catch-names-bad -DMIN=2
          -P ${CMAKE_CURRENT_SOURCE_DIR}/catch_names_check.cmake)
set_tests_properties(catch_names_finds_unmatched PROPERTIES
  PASS_REGULAR_EXPRESSION "1 of 2 TEST_CASE names have an unmatched"
  FAIL_REGULAR_EXPRESSION "Ctrl\\+\\[ and")

# No app, CLI or core message counts through tr or translate or writes "(s)"
# (#304).
add_test(NAME plural_strings
  COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/app -P ${CMAKE_CURRENT_SOURCE_DIR}/plural_strings_check.cmake)
foreach(dir cli core/src)
  string(REPLACE "/" "_" name "plural_strings_${dir}")
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/${dir} -P ${CMAKE_CURRENT_SOURCE_DIR}/plural_strings_check.cmake)
endforeach()
# and the check itself, on a fixture per form it must find or let be
add_test(NAME plural_strings_fixtures
  COMMAND ${CMAKE_COMMAND} -DCHECK=${CMAKE_CURRENT_SOURCE_DIR}/plural_strings_check.cmake
          -DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/data/plural-strings -P ${CMAKE_CURRENT_SOURCE_DIR}/plural_strings_fixtures.cmake)

# scripts/release (run on Linux): the same tarball of a commit every time,
# committed files only
if(UNIX AND NOT APPLE)
  add_test(NAME release_tarball
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/release_test.sh
            ${PROJECT_SOURCE_DIR}/scripts/release ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/release-tmp)
  set_tests_properties(release_tarball PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_test(NAME forge_merge
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/forge_merge_test.sh
          ${PROJECT_SOURCE_DIR}/scripts/forge ${CMAKE_CURRENT_BINARY_DIR}/forge-merge-tmp)
set_tests_properties(forge_merge PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(blockwright-render-tests
  test_canvas_renderer.cpp
  test_render.cpp
  test_render_golden.cpp
  test_irc_render.cpp
  test_glyph_perf.cpp
  util.h
)
target_link_libraries(blockwright-render-tests PRIVATE blockwright::render Catch2::Catch2WithMain)
target_compile_definitions(blockwright-render-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
# render/src/unifont.h: the tests check cells against Unifont's own bitmaps
target_include_directories(blockwright-render-tests PRIVATE ${PROJECT_SOURCE_DIR}/render/src)
if(NOT BLOCKWRIGHT_SYSTEM_UNIFONT)
  target_compile_definitions(blockwright-render-tests PRIVATE BW_BUNDLED_UNIFONT)
endif()
catch_discover_tests(blockwright-render-tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

add_test(NAME cli_render
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_render_test.sh
          ${BW_CLI_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/cli-render-tmp)
set_tests_properties(cli_render PROPERTIES ENVIRONMENT "${BW_CLI_ENV}")

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
set(BW_APP_TEST_RUNNER ${BW_APP_TEST_ENV} ${CMAKE_CROSSCOMPILING_EMULATOR})

add_executable(blockwright-app-tests
  test_app_wide.cpp
  test_app_fifo.cpp
  test_app.cpp
  test_app_brush.cpp
  test_app_layers.cpp
  test_app_canvas_size.cpp
  test_app_message_box.cpp
  test_app_clone_replay.cpp
  test_app_text_replay.cpp
  test_app_layer_style.cpp
  test_app_layer_move.cpp
  test_app_item_checks.cpp
  test_app_cli_shade.cpp
  test_app_draw.cpp
  test_app_frame.cpp
  test_app_frame_library.cpp
  test_app_format.cpp
  test_app_irc.cpp
  test_app_irc_target.cpp
  test_app_micron.cpp
  test_app_micron_target.cpp
  test_app_library.cpp
  test_app_library_styles.cpp
  test_app_style_apply.cpp
  test_app_lint.cpp
  test_app_select.cpp
  test_app_reference_layers.cpp
  test_app_reference_transform.cpp
  test_app_shading.cpp
  test_app_gradient.cpp
  test_app_perf.cpp
  test_app_detail.cpp
  test_app_workspace.cpp
  test_app_toolbox_access.cpp
  test_app_tool_icons.cpp
  test_app_project.cpp
  test_app_start_screen.cpp
  test_app_samples.cpp
  test_app_text.cpp
  test_app_generator.cpp
  test_app_fonts.cpp
  test_app_font_collection.cpp
  test_app_shortcuts.cpp
  test_app_shortcut_settings.cpp
  test_app_list_actions.cpp
  test_app_help.cpp
  test_app_scene.cpp
  test_app_branding.cpp
  test_app_screenshot.cpp
  scene_env.h
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
# test_app_irc.cpp runs the CLI's export on the projects the app saves.
add_dependencies(blockwright-app-tests blockwright-cli)
target_compile_definitions(blockwright-app-tests PRIVATE BW_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                        BW_BUNDLED_LIBRARY="${PROJECT_SOURCE_DIR}/library"
                                                        BW_HELP_SOURCE="${PROJECT_SOURCE_DIR}/app/help"
                                                        BW_BRANDING_DIR="${PROJECT_SOURCE_DIR}/branding"
                                                        BW_SAMPLES_SOURCE="${PROJECT_SOURCE_DIR}/samples"
                                                        BW_PACKAGING_DIR="${PROJECT_SOURCE_DIR}/packaging"
                                                        BW_ANSIDRAW_DIR="${BW_ANSIDRAW_DIR}"
                                                        BW_CLI="$<TARGET_FILE:blockwright-cli>")
set_target_properties(blockwright-app-tests PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_RUNNER}")
catch_discover_tests(blockwright-app-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)
# The default dock heights at fractional device pixel ratios: the preview's
# target is in whole device pixels there.
foreach(scale 1.25 1.5)
  add_test(NAME "dock heights at scale ${scale}"
    COMMAND ${BW_APP_TEST_ENV} QT_SCALE_FACTOR=${scale} ${CMAKE_CROSSCOMPILING_EMULATOR}
            $<TARGET_FILE:blockwright-app-tests> "[dock-height]")
endforeach()
# The IRC View dock's origin at a fractional device pixel ratio (#394): the
# canvas's cell under its viewport's corner, on the canvas's device grid.
add_test(NAME "IRC View origin at scale 1.25"
  COMMAND ${BW_APP_TEST_ENV} QT_SCALE_FACTOR=1.25 ${CMAKE_CROSSCOMPILING_EMULATOR}
          $<TARGET_FILE:blockwright-app-tests> "[irc-view-dpr]")
# The IRC row meter's rows at fractional device pixel ratios (#404): the
# canvas's row under each meter pixel, on the canvas's device grid.
foreach(scale 1.25 1.5)
  add_test(NAME "IRC meter rows at scale ${scale}"
    COMMAND ${BW_APP_TEST_ENV} QT_SCALE_FACTOR=${scale} ${CMAKE_CROSSCOMPILING_EMULATOR}
            $<TARGET_FILE:blockwright-app-tests> "[irc-meter-dpr]")
endforeach()
# A view step's scrollTo at fractional device pixel ratios (#398): the whole
# cell the step names is in view at the canvas's viewport corner.
foreach(scale 1.25 1.5 1.75)
  add_test(NAME "view step scrollTo at scale ${scale}"
    COMMAND ${BW_APP_TEST_ENV} QT_SCALE_FACTOR=${scale} ${CMAKE_CROSSCOMPILING_EMULATOR}
            $<TARGET_FILE:blockwright-app-tests> "[view-dpr]")
endforeach()
# "scale": 2 screenshots (#295) on the screens blockwright --screenshot
# fixes (screenshot::FixedScreens): one at device pixel ratio 1, one at 2.
# The first case checks these are its screens.
set(config "${CMAKE_CURRENT_BINARY_DIR}/offscreen-screenshot-screens.json")
file(WRITE "${config}"
  "{\"windowFrameMargins\": false, \"screens\": [\n"
  " {\"name\": \"scale1\", \"x\": 0, \"y\": 0, \"width\": 4096, \"height\": 4096,"
  " \"logicalDpi\": 96, \"logicalBaseDpi\": 96, \"dpr\": 1},\n"
  " {\"name\": \"scale2\", \"x\": 4096, \"y\": 0, \"width\": 8192, \"height\": 8192,"
  " \"logicalDpi\": 192, \"logicalBaseDpi\": 96, \"dpr\": 1}]}\n")
add_test(NAME "screenshots at scale 2"
  COMMAND ${BW_APP_TEST_ENV} QT_QPA_PLATFORM=offscreen:configfile=${config}
          ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:blockwright-app-tests> "[screenshot-scale]")
# A layout test that runs after a closed window in the same process still starts
# from the default layout (#249); ctest otherwise runs each test case alone.
# The script runs the binary itself, so it runs on the build host only.
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME app_tests_layout_after_a_close
    COMMAND ${BW_APP_TEST_ENV} bash ${CMAKE_CURRENT_SOURCE_DIR}/app_order_test.sh $<TARGET_FILE:blockwright-app-tests>)
endif()
# blockwright --screenshot (#295): the PNGs a steps file asks for, their
# sizes, a failing step and the usage errors, through the binary itself.
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME app_screenshot
    COMMAND ${BW_APP_TEST_ENV} sh ${CMAKE_CURRENT_SOURCE_DIR}/app_screenshot_test.sh ${BW_GUI_COMMAND}
            ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR}/app-screenshot-check)
endif()
# The canvas keeps its floor after the window shrinks (#238): a scroll bar
# appears with the shorter or narrower window, at every scale.
foreach(scale 1.25 1.5 1.75 2)
  add_test(NAME "canvas floor after a shrink at scale ${scale}"
    COMMAND ${BW_APP_TEST_ENV} QT_SCALE_FACTOR=${scale} ${CMAKE_CROSSCOMPILING_EMULATOR}
            $<TARGET_FILE:blockwright-app-tests> "[canvas-floor-shrink]")
endforeach()
# The dock column widths at fractional device pixel ratios. Some of these
# tests size the window from the screen (the first-run size, restoreGeometry
# fitting it), and the offscreen screen is 800x600 device pixels, which a
# scale factor shrinks in logical pixels (400x300 at 2). So each scale gets a
# screen of 800x600 logical pixels, the one the unscaled run has: the run
# checks the same layout in fractional device pixels, not a smaller screen.
# On a Mac the unscaled run has test_app.cpp's Mac screen, 800x800 at 72 dpi.
if(APPLE)
  set(dock_screens "1.25 1000 1000" "1.5 1200 1200" "1.75 1400 1400" "2 1600 1600")
  set(dock_dpi 72)
else()
  set(dock_screens "1.25 1000 750" "1.5 1200 900" "1.75 1400 1050" "2 1600 1200")
  set(dock_dpi 96)
endif()
foreach(screen IN LISTS dock_screens)
  separate_arguments(screen)
  list(GET screen 0 scale)
  list(GET screen 1 width)
  list(GET screen 2 height)
  set(config "${CMAKE_CURRENT_BINARY_DIR}/offscreen-unscaled-at-${scale}.json")
  file(WRITE "${config}"
    "{\"screens\": [{\"name\": \"offscreen\", \"x\": 0, \"y\": 0, \"width\": ${width}, \"height\": ${height},"
    " \"logicalDpi\": ${dock_dpi}, \"logicalBaseDpi\": ${dock_dpi}, \"dpr\": 1}]}\n")
  add_test(NAME "dock widths at scale ${scale}"
    COMMAND ${BW_APP_TEST_ENV} QT_QPA_PLATFORM=offscreen:configfile=${config} QT_SCALE_FACTOR=${scale}
            ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:blockwright-app-tests> "[dock-width]")
endforeach()

# The launch start screen on two screens of different heights: it comes up
# on the window's screen, whichever the pointer is on, and fits it.
set(config "${CMAKE_CURRENT_BINARY_DIR}/offscreen-two-screens.json")
file(WRITE "${config}"
  "{\"screens\": [{\"name\": \"small\", \"x\": 0, \"y\": 0, \"width\": 1366, \"height\": 768,"
  " \"logicalDpi\": 96, \"logicalBaseDpi\": 96, \"dpr\": 1},"
  " {\"name\": \"big\", \"x\": 1366, \"y\": 0, \"width\": 2560, \"height\": 1440,"
  " \"logicalDpi\": 96, \"logicalBaseDpi\": 96, \"dpr\": 1}]}\n")
add_test(NAME "start screen on two screens"
  COMMAND ${BW_APP_TEST_ENV} QT_QPA_PLATFORM=offscreen:configfile=${config}
          ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:blockwright-app-tests> "[start-screen-screens]")

# ... and on a work area shorter than the screen, its top-left stays on it.
set(config "${CMAKE_CURRENT_BINARY_DIR}/offscreen-short-screen.json")
file(WRITE "${config}"
  "{\"screens\": [{\"name\": \"short\", \"x\": 0, \"y\": 0, \"width\": 1024, \"height\": 574,"
  " \"logicalDpi\": 96, \"logicalBaseDpi\": 96, \"dpr\": 1}]}\n")
add_test(NAME "start screen on a short screen"
  COMMAND ${BW_APP_TEST_ENV} QT_QPA_PLATFORM=offscreen:configfile=${config}
          ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:blockwright-app-tests> "[start-screen-short]")

# The app tests once more with macOS's message boxes, the title as the heading
# and the message as the informative text, so a test that reads a box's
# message from text() fails here and not first on a Mac.
if(NOT APPLE)
  add_test(NAME blockwright-app-tests_heading_boxes
    COMMAND ${BW_APP_TEST_ENV} BW_TEST_HEADING_BOXES=1 ${CMAKE_CROSSCOMPILING_EMULATOR}
            $<TARGET_FILE:blockwright-app-tests>)
  # The whole app suite in one process: 135 to 195 s in CI, a little more
  # under Qt 6.7, and over 300 s under either on a loaded runner. 1500 is
  # the ctest default, so no build that runs ctest without --timeout gives
  # it less time than before.
  set_tests_properties(blockwright-app-tests_heading_boxes PROPERTIES TIMEOUT 1500)
endif()

# On an X server of their own, with input through XTest: skipped where Xvfb or
# the xcb libraries aren't installed. They can't run offscreen, so the
# offscreen profile check can't run them; xcb_test.sh gives each run an empty
# HOME and XDG profile of its own and fails if the run leaves anything in it.
# It is written into the test's command: CMake drops a CROSSCOMPILING_EMULATOR
# from add_test() in a native build under CMP0158, and from a $<TARGET_FILE>
# command under either setting.
set(BW_APP_XCB_TEST_ENV bash ${CMAKE_CURRENT_SOURCE_DIR}/xcb_test.sh ${CMAKE_CURRENT_BINARY_DIR}/app-xcb-profile)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
  pkg_check_modules(XCB_TEST IMPORTED_TARGET xcb xcb-xtest)
endif()
if(XCB_TEST_FOUND AND NOT WIN32)
  add_executable(blockwright-app-xcb-tests test_app_xcb.cpp scratch_settings.h)
  target_link_libraries(blockwright-app-xcb-tests PRIVATE blockwright_app Catch2::Catch2 PkgConfig::XCB_TEST)
  add_test(NAME app_xcb COMMAND ${BW_APP_XCB_TEST_ENV} $<TARGET_FILE:blockwright-app-xcb-tests>)
  set_tests_properties(app_xcb PROPERTIES SKIP_RETURN_CODE 77)
endif()
# xcb_test.sh keeps a run out of the caller's profile and fails one that
# writes into its own.
if(NOT CMAKE_CROSSCOMPILING)  # X on the host
add_test(NAME xcb_runner
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/xcb_runner_test.sh ${CMAKE_CURRENT_SOURCE_DIR}/xcb_test.sh
          ${CMAKE_CURRENT_BINARY_DIR}/xcb-runner-check)
set_tests_properties(xcb_runner PROPERTIES SKIP_RETURN_CODE 77)
endif()

# At a fractional device pixel ratio: its main sets QT_SCALE_FACTOR before the
# QApplication exists.
add_executable(blockwright-app-scale-tests
  test_app_scale.cpp
  scratch_settings.h
)
target_link_libraries(blockwright-app-scale-tests PRIVATE blockwright_app Catch2::Catch2)
set_target_properties(blockwright-app-scale-tests PROPERTIES CROSSCOMPILING_EMULATOR "${BW_APP_TEST_RUNNER}")
catch_discover_tests(blockwright-app-scale-tests
  PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen
  DISCOVERY_MODE PRE_TEST)

# QSettings construction belongs in the settings factory. This secondary
# token guard checks direct construction, value template arguments/defaults
# and local macro arguments in any position (including cv-qualified and
# elaborated class types, and after earlier arguments holding ->, a
# comparison >, or < inside nested (), [] or {} groups: comparisons, shifts
# and operator< / operator<<), aliases, Qt global-static macros and bases in
# any position with or without access/virtual. Only * and & exempt type arguments/aliases,
# including in a base's template arguments; explicit smart-pointer types are
# reported. Plain template-return declarations ending directly in ); remain
# exempt, as do includes and __has_include operands. It doesn't resolve types
# or expand macros; the scratch settings runtime check and profile test remain
# the binding guards.
add_executable(bw-settings-guard settings_guard.cpp)
add_test(NAME app_settings_factory
  COMMAND bw-settings-guard --allow ${PROJECT_SOURCE_DIR}/app/settings.cpp
          ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/render ${PROJECT_SOURCE_DIR}/cli)
add_test(NAME app_settings_guard_fixtures
  COMMAND bw-settings-guard --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/data/settings-guard)

# The profile checks below run on the host's X11/XDG profile and build
# projects with the host compiler: a cross build leaves them out.
if(NOT CMAKE_CROSSCOMPILING)
# Run whole under a fresh HOME and XDG profile, the app test binaries leave it
# empty: Blockwright's settings go to the factory's scratch INI file and Qt's
# own (the QtProject store QFileDialog keeps its history in) to the scratch
# directory as well.
set(BW_APP_TEST_BINARIES blockwright-app-tests blockwright-app-scale-tests)
set(bw_app_test_files ${BW_APP_TEST_BINARIES})
list(TRANSFORM bw_app_test_files REPLACE "^(.+)$" "$<TARGET_FILE:\\1>")
add_test(NAME app_tests_leave_profile_alone
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_profile_test.sh ${CMAKE_CURRENT_BINARY_DIR}/app-profile-check
          ${bw_app_test_files})
# It runs two whole app test binaries in sequence.
set_tests_properties(app_tests_leave_profile_alone PROPERTIES TIMEOUT 3000)
# It leaves fontconfig's cache out and still finds anything else written.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake/fontconfig.sh
  "mkdir -p \"$XDG_CACHE_HOME/fontconfig\" && touch \"$XDG_CACHE_HOME/fontconfig/x-le64.cache-9\"\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake/settings.sh
  "mkdir -p \"$XDG_CONFIG_HOME/QtProject\" \"$XDG_CACHE_HOME/fontconfig\" && touch \"$XDG_CONFIG_HOME/QtProject/x.conf\"\n")
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake/fontconfig.sh ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake/settings.sh
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
add_test(NAME app_profile_ignores_fontconfig_cache
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_profile_test.sh ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake-check
          ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake/fontconfig.sh ${CMAKE_CURRENT_BINARY_DIR}/app-profile-fake/settings.sh)
set_tests_properties(app_profile_ignores_fontconfig_cache PROPERTIES
  PASS_REGULAR_EXPRESSION "ok   fontconfig.sh\nFAIL settings.sh wrote into the profile:\n  ./config\n  ./config/QtProject\n  ./config/QtProject/x.conf\n"
  FAIL_REGULAR_EXPRESSION "  ./cache")
# Each app test binary, when its scratch directory can't be made, refuses to
# run and leaves a profile already holding Blockwright settings byte-identical:
# the settings would otherwise be that profile's own. Linux paths, so host only.
# The X tests' binary takes the same path through scratch_settings.h; only
# xcb_test.sh may run it (app_tests_all_guarded), so it isn't run here.
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME app_tests_refuse_without_scratch
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_refusal_test.sh ${CMAKE_CURRENT_BINARY_DIR}/app-refusal-check
            ${bw_app_test_files})
endif()

# Every test binary built on the app is in that list and runs under the
# scratch profile, or every test that runs it runs it on an X server through
# xcb_test.sh, so one added later can't be left out of both.
include(${CMAKE_CURRENT_SOURCE_DIR}/app_guard.cmake)
bw_add_app_guard_test()
add_test(NAME app_guard_fixtures
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_guard_fixture_test.sh ${CMAKE_COMMAND} ${CMAKE_CTEST_COMMAND}
          "${CMAKE_GENERATOR}" ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_BINARY_DIR}/app-guard-fixtures ${PROJECT_SOURCE_DIR})
# And under a multi-config generator, where a nested ctest without -C finds the
# guard NOT_AVAILABLE
if(CMAKE_GENERATOR MATCHES "^Ninja")
  add_test(NAME app_guard_fixtures_multi_config
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_guard_fixture_test.sh ${CMAKE_COMMAND} ${CMAKE_CTEST_COMMAND}
            "Ninja Multi-Config" ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_BINARY_DIR}/app-guard-fixtures-multi
            ${PROJECT_SOURCE_DIR})
  # and with CMAKE_CONFIGURATION_TYPES in the environment naming only another
  # configuration than the one the nested ctest asks for
  add_test(NAME app_guard_fixtures_multi_config_debug_env
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/app_guard_fixture_test.sh ${CMAKE_COMMAND} ${CMAKE_CTEST_COMMAND}
            "Ninja Multi-Config" ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_BINARY_DIR}/app-guard-fixtures-multi-debug
            ${PROJECT_SOURCE_DIR})
  set_tests_properties(app_guard_fixtures_multi_config_debug_env PROPERTIES
    ENVIRONMENT CMAKE_CONFIGURATION_TYPES=Debug)
endif()
endif()

add_test(NAME gui_version
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/gui_version_test.sh ${BW_GUI_COMMAND})
set_property(TEST gui_version APPEND PROPERTY ENVIRONMENT "BW_LOGO=${PROJECT_SOURCE_DIR}/branding/logo.txt")
set_property(TEST gui_version APPEND PROPERTY ENVIRONMENT "BW_VERSION=${PROJECT_VERSION}")
if(BLOCKWRIGHT_SYSTEM_UNIFONT)
  set_property(TEST gui_version APPEND PROPERTY ENVIRONMENT
    "BW_UNIFONT_CREDIT=Includes ${BW_UNIFONT_RELEASE} Roman Czyborra, Paul Hardy et al.,")
endif()

# blockwright --list-actions (#209) with no display and an empty HOME and XDG
# profile: JSON, unique ids, nothing written. The profile is the host's, so a
# cross build leaves it out.
if(BW_PYTHON3 AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME gui_list_actions
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/list_actions_test.sh ${CMAKE_CURRENT_BINARY_DIR}/list-actions-check
            ${BW_PYTHON3} ${BW_GUI_COMMAND})
endif()

# The app bundle (macOS, BLOCKWRIGHT_MACOS_BUNDLE): its Info.plist, and the
# notices --version names inside it.
if(BLOCKWRIGHT_MACOS_BUNDLE)
  add_test(NAME macos_bundle
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/macos_bundle_test.sh $<TARGET_BUNDLE_DIR:blockwright>
            ${PROJECT_VERSION} ${BW_BUNDLE_ID} ${CMAKE_OSX_DEPLOYMENT_TARGET})
  set_property(TEST gui_version install_notices APPEND PROPERTY ENVIRONMENT "BW_NOTICES_DIR=${BW_NOTICES_DIR}")
endif()

# scripts/package-macos refuses a Qt built for a newer macOS than the target
if(APPLE)
  add_test(NAME macos_package_qt
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/macos_package_qt_test.sh ${PROJECT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/macos-package-qt-check)
  set_tests_properties(macos_package_qt PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(WIN32)
  # the UTF-8 code page and long paths, as the shipped programs have
  foreach(t blockwright-tests blockwright-render-tests blockwright-app-tests blockwright-app-scale-tests
            bw-settings-guard bw-gen-helper)
    bw_windows_resources(${t})
  endforeach()
endif()

# no two test cases in a file share a scratch directory, which ctest -j would
# let them empty under each other
add_test(NAME scratch_names
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/scratch_names_check.cmake)
