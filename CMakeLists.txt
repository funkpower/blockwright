cmake_minimum_required(VERSION 3.24)
# The oldest macOS Qt 6.8 runs on; read only when building for a Mac. A
# MACOSX_DEPLOYMENT_TARGET in the environment, or -D, names another.
if(NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Oldest macOS the build runs on")
endif()
project(blockwright VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
# libstdc++'s cheap precondition checks (std::clamp's bounds, an index past
# a vector or string) in the builds the tests run in, as most distributions
# build with; a Release build is left as it is. libc++ ignores the macro.
add_compile_definitions($<$<CONFIG:Debug,RelWithDebInfo>:_GLIBCXX_ASSERTIONS>)

# macOS: with BLOCKWRIGHT_MACOS_BUNDLE the GUI is an app bundle and everything
# installs inside it: the CLI beside the GUI in Contents/MacOS, the Library and
# the notices in Contents/Resources (packaging/README.md). Without it a
# Mac build installs bin/ and share/ as on Linux.
if(APPLE)
  option(BLOCKWRIGHT_MACOS_BUNDLE "Build the GUI as an app bundle holding the CLI, the Library and the notices" OFF)
  # Installed, the binaries load Qt from where they were linked against it
  # (Qt's frameworks are @rpath ones, and no Qt install is on dyld's default
  # path); a bundle's look in Contents/Frameworks first.
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()
if(BLOCKWRIGHT_MACOS_BUNDLE)
  set(BW_BUNDLE_ID online.pumpin.funkpower.blockwright)
  set(CMAKE_INSTALL_BINDIR Blockwright.app/Contents/MacOS)
  set(CMAKE_INSTALL_DATADIR Blockwright.app/Contents/Resources)
  set(CMAKE_INSTALL_DOCDIR Blockwright.app/Contents/Resources/doc)
  # where macdeployqt puts Qt
  set(CMAKE_INSTALL_RPATH @executable_path/../Frameworks)
  # Where --version and the About box send the reader for the notices
  # (blockwright/credits.h).
  set(BW_NOTICES_DIR "Contents/Resources/doc/ in the app bundle")
  add_compile_definitions(BW_NOTICES_DIR="${BW_NOTICES_DIR}")
endif()

include(GNUInstallDirs)
# A cross build's tests run under wine (cmake/Windows.cmake), where a program
# that hangs holds up the whole run: each test gets 300 s unless it sets its
# own TIMEOUT, not CTest's 1500.
if(WIN32 AND CMAKE_CROSSCOMPILING)
  set(DART_TESTING_TIMEOUT 300 CACHE STRING "Maximum time allowed before CTest will kill the test.")
endif()
# BUILD_TESTING (CTest's switch, default ON) builds the tests, which need
# Catch2 3; OFF builds without them and without Catch2.
include(CTest)
# Distribution builders have no network: with BW_FETCH_CATCH2 OFF the tests
# use the installed Catch2 3 or the configure stops and says so.
option(BW_FETCH_CATCH2 "Download Catch2 when no installed Catch2 3 is found" ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg)
# Older Qt releases (6.8.3 among them) link the AGL framework, which the
# macOS 26 SDK no longer has; Qt uses nothing in it.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
  get_target_property(bw_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
  if(bw_gl_libs)
    list(FILTER bw_gl_libs EXCLUDE REGEX "AGL")
    set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${bw_gl_libs}")
  endif()
endif()
# Project files (PLAN §6): zip containers, zstd layer cells. zstd 1.5.6 and
# later name the library zstd::libzstd; earlier configs only the shared and
# static targets.
find_package(libzip 1.7 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
if(TARGET zstd::libzstd)
  set(BW_ZSTD zstd::libzstd)
elseif(TARGET zstd::libzstd_shared)
  set(BW_ZSTD zstd::libzstd_shared)
else()
  set(BW_ZSTD zstd::libzstd_static)
endif()

if(WIN32)
  enable_language(RC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  include(cmake/Windows.cmake)
endif()

# The short commit reported by --version. Empty means ask git on every build,
# so the binary never reports a stale commit; scripts/install-dev sets it
# because it builds from an exported tree with no .git.
set(BLOCKWRIGHT_GIT_COMMIT "" CACHE STRING "Commit reported by --version (empty: ask git at build time)")
# A tree made by `git archive` (scripts/release, or the forge's archive of a
# tag) has no .git; git wrote the commit into packaging/tarball-commit
# (export-subst). Read it rather than ask git, which would answer for whatever
# repository the tree was unpacked inside: a packaging repository's own.
if(NOT BLOCKWRIGHT_GIT_COMMIT AND EXISTS ${CMAKE_SOURCE_DIR}/packaging/tarball-commit)
  file(STRINGS ${CMAKE_SOURCE_DIR}/packaging/tarball-commit bw_tarball_commit LIMIT_COUNT 1)
  if(bw_tarball_commit MATCHES "^[0-9a-f]+$")
    set(BLOCKWRIGHT_GIT_COMMIT ${bw_tarball_commit})
  endif()
endif()
set(BW_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/blockwright/version.h)
add_custom_target(bw_version ALL
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_SOURCE_DIR}
          -DOUT=${BW_VERSION_HEADER}
          -DVERSION=${PROJECT_VERSION}
          -DCOMMIT=${BLOCKWRIGHT_GIT_COMMIT}
          -P ${CMAKE_SOURCE_DIR}/cmake/WriteVersion.cmake
  BYPRODUCTS ${BW_VERSION_HEADER}
  COMMENT "Recording the git commit for --version"
  VERBATIM)

# The plain-text logo (branding/logo.txt, #205) that --help and --version
# print, as a header: generated/blockwright/logo_text.h. Editing the file
# reconfigures.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/branding/logo.txt)
file(READ ${CMAKE_SOURCE_DIR}/branding/logo.txt BW_LOGO_TEXT)
string(REPLACE "\r" "" BW_LOGO_TEXT "${BW_LOGO_TEXT}")
configure_file(cmake/logo_text.h.in ${CMAKE_BINARY_DIR}/generated/blockwright/logo_text.h @ONLY)

# The glyphs CP437 lacks draw from GNU Unifont (PLAN §16, Rendering), built
# from third_party/unifont/ unless this names the directory of the
# distribution's Unifont .hex files (the BMP set, unifont.hex or
# unifont-VERSION.hex, and the upper planes, unifont_upper or unifont_all
# likewise, each plain or gzipped; scripts/gen-unifont --help), which the
# Debian package uses: /usr/share/unifont, with unifont.hex and
# unifont_all.hex. The rpm and AUR packages build from third_party/unifont/.
set(BLOCKWRIGHT_SYSTEM_UNIFONT "" CACHE PATH "Build Unifont from the .hex files in this directory, not third_party/unifont/")

add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(cli)
add_subdirectory(app)
add_subdirectory(samples)
add_subdirectory(library-maker)

# The licence and the bundled fonts' and icons' notices travel with the
# binaries (CC BY-SA 4.0 section 3, MIT, OFL 1.1): <prefix>/share/doc/blockwright/.
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/oldschool-pc-fonts/LICENSE third_party/oldschool-pc-fonts/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/oldschool-pc-fonts)
install(FILES third_party/tabler-icons/LICENSE third_party/tabler-icons/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/tabler-icons)
install(FILES third_party/unifont/OFL-1.1.txt third_party/unifont/COPYING third_party/unifont/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/unifont)

# The desktop entry, icon, AppStream metadata and manual pages of a Linux
# (freedesktop) install, which the distribution packages and the AppImage carry.
# packaging/linux/blockwright-*.png are rendered from branding/icon.ans and
# branding/icon-small.ans by branding/render.
if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/blockwright.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/linux/online.pumpin.funkpower.blockwright.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  foreach(size 16 32 48 128 256 512)
    install(FILES packaging/linux/blockwright-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME blockwright.png)
  endforeach()
  install(FILES packaging/linux/blockwright.1 packaging/linux/blockwright-cli.1
          DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

# The bundled Library (PLAN §12): stamps the GUI and `blockwright-cli library
# list` find from the binaries' directory, <prefix>/share/blockwright/library/.
install(DIRECTORY library/ DESTINATION ${CMAKE_INSTALL_DATADIR}/blockwright/library)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
