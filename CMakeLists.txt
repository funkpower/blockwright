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

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui)

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

# The licence and the bundled font's notices travel with the binaries
# (CC BY-SA 4.0 section 3): <prefix>/share/doc/blockwright/.
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/oldschool-pc-fonts/LICENSE third_party/oldschool-pc-fonts/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/oldschool-pc-fonts)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
