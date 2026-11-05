# The renderer: documents to images with the bundled VGA font, and GNU Unifont
# for the glyphs CP437 lacks. QtGui only; it must never link QtWidgets.
add_library(blockwright_render STATIC
  src/canvas_renderer.cpp
  src/irc_render.cpp
  src/render.cpp
  src/unifont.cpp
  src/unifont.h
)
add_library(blockwright::render ALIAS blockwright_render)
target_include_directories(blockwright_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright_render PUBLIC blockwright::core Qt6::Gui)
target_compile_options(blockwright_render PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

# Unifont (PLAN §16, Rendering): third_party/unifont/unifont.bin.z, or with
# BLOCKWRIGHT_SYSTEM_UNIFONT the same table made by scripts/gen-unifont from
# the distribution's .hex files. Compiled in as :/blockwright/unifont.bin.z,
# already compressed.
if(BLOCKWRIGHT_SYSTEM_UNIFONT)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  # gen-unifont picks the files (its --help); these are what it may read.
  file(GLOB bw_unifont_hex CONFIGURE_DEPENDS
    ${BLOCKWRIGHT_SYSTEM_UNIFONT}/unifont*.hex ${BLOCKWRIGHT_SYSTEM_UNIFONT}/unifont*.hex.gz)
  if(NOT bw_unifont_hex)
    message(FATAL_ERROR "BLOCKWRIGHT_SYSTEM_UNIFONT: no Unifont .hex files in ${BLOCKWRIGHT_SYSTEM_UNIFONT}")
  endif()
  set(BW_UNIFONT_DIR ${CMAKE_CURRENT_BINARY_DIR}/unifont)
  add_custom_command(OUTPUT ${BW_UNIFONT_DIR}/unifont.bin.z
    COMMAND Python3::Interpreter -I ${PROJECT_SOURCE_DIR}/scripts/gen-unifont ${BLOCKWRIGHT_SYSTEM_UNIFONT}
            -o ${BW_UNIFONT_DIR}/unifont.bin.z
    DEPENDS ${PROJECT_SOURCE_DIR}/scripts/gen-unifont ${bw_unifont_hex}
    COMMENT "Building the Unifont table from ${BLOCKWRIGHT_SYSTEM_UNIFONT}"
    VERBATIM)
  # The release the files name, if they name one (Debian's don't); the
  # copyright years are the bundled release's, so they are left out too.
  execute_process(COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/scripts/gen-unifont --release
                          ${BLOCKWRIGHT_SYSTEM_UNIFONT}
    OUTPUT_VARIABLE bw_unifont_version OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE bw_unifont_status)
  if(NOT bw_unifont_status EQUAL 0)
    message(FATAL_ERROR "BLOCKWRIGHT_SYSTEM_UNIFONT: gen-unifont --release failed on ${BLOCKWRIGHT_SYSTEM_UNIFONT}")
  endif()
  if(bw_unifont_version)
    set(BW_UNIFONT_RELEASE "GNU Unifont ${bw_unifont_version}, (c)")
  else()
    set(BW_UNIFONT_RELEASE "GNU Unifont, (c)")
  endif()
else()
  set(BW_UNIFONT_DIR ${PROJECT_SOURCE_DIR}/third_party/unifont)
  # The release and its copyright years, from the bundled copy's README
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BW_UNIFONT_DIR}/README.md)
  file(STRINGS ${BW_UNIFONT_DIR}/README.md bw_unifont_heading LIMIT_COUNT 1 REGEX "^# GNU Unifont ")
  file(STRINGS ${BW_UNIFONT_DIR}/README.md bw_unifont_copyright ENCODING UTF-8 LIMIT_COUNT 1 REGEX "^- [*][*]Copyright:[*][*] ")
  string(REGEX MATCH "^# GNU Unifont ([0-9][0-9.]*)$" bw_unifont_heading "${bw_unifont_heading}")
  set(bw_unifont_version ${CMAKE_MATCH_1})
  string(REGEX MATCH " ([0-9]+-[0-9]+) " bw_unifont_copyright "${bw_unifont_copyright}")
  set(bw_unifont_years ${CMAKE_MATCH_1})
  if(NOT bw_unifont_version OR NOT bw_unifont_years)
    message(FATAL_ERROR "third_party/unifont/README.md names no release or copyright years")
  endif()
  set(BW_UNIFONT_RELEASE "GNU Unifont ${bw_unifont_version}, (c) ${bw_unifont_years}")
endif()
# For the --version tests
set(BW_UNIFONT_RELEASE ${BW_UNIFONT_RELEASE} PARENT_SCOPE)
configure_file(${PROJECT_SOURCE_DIR}/cmake/unifont_release.h.in
               ${CMAKE_BINARY_DIR}/generated/blockwright/unifont_release.h @ONLY)
qt_add_resources(blockwright_render "unifont" PREFIX "/blockwright" BASE ${BW_UNIFONT_DIR}
  OPTIONS -no-compress
  FILES ${BW_UNIFONT_DIR}/unifont.bin.z)
