# The core library: cell model, palette, CP437, ANSI, XBIN/BIN and SAUCE codecs, IRC
# text (mIRC colours and the row encoder), ramps, brushes and the lint.
# Plain C++20 (QtCore for the Library, tag case folding, projects' JSON and
# running generators; libzip and zstd for project files); it must never link
# QtWidgets.
add_library(blockwright_core STATIC
  src/ansi.cpp
  src/brush.cpp
  src/brushes/clone.cpp
  src/brushes/despeckle.cpp
  src/brushes/distance.cpp
  src/brushes/detail.cpp
  src/brushes/drawing.cpp
  src/brushes/form.cpp
  src/brushes/gradient.cpp
  src/brushes/guard.cpp
  src/brushes/shading.cpp
  src/brushes/smudge.cpp
  src/brushes/soft.cpp
  src/brushes/texture.cpp
  src/brushes/spray.cpp
  src/canvas.cpp
  src/cp437.cpp
  src/edit.cpp
  src/face.cpp
  src/frame.cpp
  src/font_store.cpp
  src/tdf.cpp
  src/generator.cpp
  src/glyph.cpp
  src/glyph_rows.cpp
  src/glyph_sets.cpp
  src/canvas_size.cpp
  src/composite.cpp
  src/halfblock.cpp
  src/irc.cpp
  src/irc_view.cpp
  src/layer.cpp
  src/library.cpp
  src/links.cpp
  src/lint.cpp
  src/lint_fallback.cpp
  src/lint_rules.cpp
  src/micron.cpp
  src/mirc.cpp
  src/palette.cpp
  src/palette_swap.cpp
  src/pc_fonts.cpp
  src/project.cpp
  src/regular_file.cpp
  src/ramp.cpp
  src/samples.cpp
  src/sauce.cpp
  src/target.cpp
  src/scene_import.cpp
  src/selection.cpp
  src/shapes.cpp
  src/steps.cpp
  src/stamp_slice.cpp
  src/big_text.cpp
  src/text.cpp
  src/text_style.cpp
  src/width.cpp
  src/xbin.cpp
)
add_library(blockwright::core ALIAS blockwright_core)
target_include_directories(blockwright_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Test instrumentation (work counters only tests read) is compiled in only when
# the tests are built; a BUILD_TESTING=OFF build carries none of it.
if(BUILD_TESTING)
  target_compile_definitions(blockwright_core PUBLIC BW_TEST_HOOKS)
endif()
# The Library (library.cpp) reads JSON sidecars and finds the app data
# directory through QtCore, and projects (project.cpp) read and write their
# JSON with it, which PLAN §4 allows in core. Projects are zip files (libzip) whose layer cells
# are zstd frames (found in the top-level CMakeLists).
target_link_libraries(blockwright_core PRIVATE Qt6::Core libzip::zip ${BW_ZSTD})
# Where the bundled library is installed, from the binaries' directory.
file(RELATIVE_PATH BW_LIBRARY_FROM_BINDIR ${CMAKE_INSTALL_FULL_BINDIR}
     ${CMAKE_INSTALL_FULL_DATADIR}/blockwright/library)
set_source_files_properties(src/library.cpp PROPERTIES
  COMPILE_DEFINITIONS "BW_LIBRARY_FROM_BINDIR=\"${BW_LIBRARY_FROM_BINDIR}\"")
# And the sample projects, beside it.
file(RELATIVE_PATH BW_SAMPLES_FROM_BINDIR ${CMAKE_INSTALL_FULL_BINDIR}
     ${CMAKE_INSTALL_FULL_DATADIR}/blockwright/samples)
set_source_files_properties(src/samples.cpp PROPERTIES
  COMPILE_DEFINITIONS "BW_SAMPLES_FROM_BINDIR=\"${BW_SAMPLES_FROM_BINDIR}\"")
# The tests check the lookup against the same path, whatever the layout.
set(BW_SAMPLES_FROM_BINDIR "${BW_SAMPLES_FROM_BINDIR}" PARENT_SCOPE)
target_compile_options(blockwright_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
