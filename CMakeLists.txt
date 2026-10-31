cmake_minimum_required(VERSION 3.24)
project(blockwright VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg)
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

add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(cli)
add_subdirectory(app)

# The licence and the bundled font's and icons' notices travel with the
# binaries (CC BY-SA 4.0 section 3, MIT): <prefix>/share/doc/blockwright/.
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/oldschool-pc-fonts/LICENSE third_party/oldschool-pc-fonts/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/oldschool-pc-fonts)
install(FILES third_party/tabler-icons/LICENSE third_party/tabler-icons/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/tabler-icons)

# The bundled Library (PLAN §12): stamps the GUI and `blockwright-cli library
# list` find from the binaries' directory, <prefix>/share/blockwright/library/.
install(DIRECTORY library/ DESTINATION ${CMAKE_INSTALL_DATADIR}/blockwright/library)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
